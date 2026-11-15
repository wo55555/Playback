# Building Playback

[Back to README](../README.md) | [简体中文](building.zh-CN.md)

## Requirements

- Visual Studio 2022 with the MSVC C++ toolchain
- [xmake](https://xmake.io/)
- Git

Playback currently targets the Windows x64 LeviLamina client runtime. The dependency versions declared in `xmake.lua` must remain aligned with the target Minecraft and LeviLamina release line.

## Release Build

Choose and check out the branch for the target runtime before configuring:

| Source branch | Minecraft / LeviLamina | Playback version |
| --- | --- | --- |
| `main`, `feat/26.10` | `26.10.*` | `0.4.0-mc26.10` |
| `develop`, `feat/26.20` | `26.20.*` | `0.4.0-mc26.20` |
| `feat/26.40` | `>=26.40.5 <26.50.0` | `0.4.0-mc26.40` |
| `feat/26.50` | `26.51.*` | `0.4.0-mc26.50` |
| `feat/26.60` | Minecraft `1.26.60.5`, LeviLamina `26.60.*` | `0.4.0-mc26.60` |

All five release lines are prereleases. Setting `PLAYBACK_VERSION` changes only the packaged version identifier, not the source branch or SDK target. The `mc26.50` suffix deliberately targets game version 26.51; the 26.60 build supports only the exact game build shown above.

The packaged mod version comes from the nearest Git release tag. Set `PLAYBACK_VERSION` explicitly when validating a release before its tag exists.

Before publishing a release, keep these declarations aligned:

- `version` in `tooth.json`.
- The current entry and comparison links in `CHANGELOG.md`.
- The release badge, compatibility table, installation command, and compatibility notes in the English and Chinese documentation.
- The resource-pack header and module versions in `resources/manifest.json` only when the resource pack itself changes semantically.

External schema fields such as Tooth `format_version`, resource-pack `format_version`, and VS Code configuration versions are not Playback release versions and must not be changed as part of a version bump.

From the repository root, configure and build a clean Release client target. This example uses the 26.20 line; use the version from the table for your checked-out branch:

```powershell
$env:PLAYBACK_VERSION = 'v0.4.0-mc26.20'
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

The packaged mod is written to `bin/playback/`. Native translations are copied to `bin/playback/lang/`, the icon font is copied to `bin/playback/fonts/`, and the lightweight main-menu button pack is installed under `bin/playback/resource_packs/playback-ui/`. The same button pack is generated as `bin/playback-ui.mcpack` for standalone manual import; it does not contain the replay browser, which is rendered natively.

Xmake builds the pinned FFmpeg 7.1 command-line runtime with x264 and copies the static executable to `bin/playback/tools/ffmpeg.exe`. It provides H.264 video and AAC, Opus, and FLAC audio encoding. Release users do not need to install FFmpeg separately. The first source build downloads and compiles this toolchain, so dependency setup takes longer than subsequent cached builds.

After building, verify `bin/playback/manifest.json` matches the selected version (here `0.4.0-mc26.20`), confirm `bin/playback/tools/ffmpeg.exe` exists, and run `git diff --check`. For runtime changes, record a fresh replay on each target version and export short 30 FPS MP4 and PNG clips with standard/Fancy graphics. Check audio-enabled exports for complete audio and test the changed options; renderer-specific changes also need tests on the affected graphics path.

The release workflow builds the tagged source with `PLAYBACK_VERSION` set to the tag, reads that version's notes from `CHANGELOG.md`, and uploads `Playback-client-windows-x64.zip` and `playback-ui.mcpack`. Keep each tag on its matching version branch. Preparing a local commit or tag does not publish it; publishing a GitHub Release triggers the workflow.

## Refresh Dependencies

If prelink reports that `bedrock_runtime_data` cannot be found, refresh the package configuration and rebuild:

```powershell
xmake repo -u
xmake f -c -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

Before submitting changes, follow the formatting and validation requirements in [CONTRIBUTING.md](../CONTRIBUTING.md).
