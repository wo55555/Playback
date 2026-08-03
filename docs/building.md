# Building Playback

[Back to README](../README.md) | [简体中文](building.zh-CN.md)

## Requirements

- Visual Studio 2022 with the MSVC C++ toolchain
- [xmake](https://xmake.io/)
- Git

Playback currently targets the Windows x64 LeviLamina client runtime. The dependency versions declared in `xmake.lua` must remain aligned with the target Minecraft and LeviLamina release line.

## Release Build

From the repository root, configure and build a clean Release client target:

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

The packaged mod is written to `bin/playback/`. Translations are copied to `bin/playback/lang/`, and the icon font is copied to `bin/playback/fonts/`. The legacy UI resource pack and standalone `.mcpack` are no longer generated.

## Refresh Dependencies

If prelink reports that `bedrock_runtime_data` cannot be found, refresh the package configuration and rebuild:

```powershell
xmake repo -u
xmake f -c -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

Before submitting changes, follow the formatting and validation requirements in [CONTRIBUTING.md](../CONTRIBUTING.md).
