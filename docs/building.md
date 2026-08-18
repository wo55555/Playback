# Building Playback

[Back to README](../README.md) | [简体中文](building.zh-CN.md)

## Requirements

- Visual Studio 2022 with the MSVC C++ toolchain
- [xmake](https://xmake.io/)
- Git

Playback currently targets the Windows x64 LeviLamina client runtime. The dependency versions declared in `xmake.lua` must remain aligned with the target Minecraft and LeviLamina release line.

## Release Build

Before building a release, keep these version declarations aligned:

- `mod_version` in `xmake.lua`.
- `version` in `tooth.json`.
- The resource-pack header and module versions in `resources/manifest.json`.
- The current entry and comparison link in `CHANGELOG.md`.

External schema fields such as Tooth `format_version`, resource-pack `format_version`, and VS Code configuration versions are not Playback release versions and must not be changed as part of a version bump.

From the repository root, configure and build a clean Release client target:

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

The packaged mod is written to `bin/playback/`. Native translations are copied to `bin/playback/lang/`, the icon font is copied to `bin/playback/fonts/`, and the lightweight main-menu button pack is installed under `bin/playback/resource_packs/playback-ui/`. The same button pack is generated as `bin/playback-ui.mcpack` for standalone manual import; it does not contain the replay browser, which is rendered natively.

Xmake builds the pinned FFmpeg 7.1 command-line runtime with x264 and copies the static executable to `bin/playback/tools/ffmpeg.exe`. Release users do not need to install FFmpeg separately. The first source build downloads and compiles this toolchain, so dependency setup takes longer than subsequent cached builds.

After building, verify `bin/playback/manifest.json` reports `0.2.0-mc26.10`, confirm `bin/playback/tools/ffmpeg.exe` exists, and run `git diff --check`. Runtime-sensitive releases should also export a short PNG sequence and MP4 on the supported renderer paths.

## Refresh Dependencies

If prelink reports that `bedrock_runtime_data` cannot be found, refresh the package configuration and rebuild:

```powershell
xmake repo -u
xmake f -c -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

Before submitting changes, follow the formatting and validation requirements in [CONTRIBUTING.md](../CONTRIBUTING.md).
