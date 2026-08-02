# Third-Party Notices

Playback uses the following third-party projects. Their licenses remain with their respective copyright holders.

| Project | Version or range | License |
| --- | --- | --- |
| [LeviLamina](https://github.com/LiteLDev/LeviLamina) | `26.10.*` | LGPL-3.0 for non-closed-source portions |
| [SymbolProvider](https://github.com/LiteLDev/SymbolProvider) | `1.2.0` (via LeviLamina) | Public Domain statement in source; no standalone license file |
| [Dear ImGui](https://github.com/ocornut/imgui) | `1.92.7` | MIT |
| [libzip](https://github.com/nih-at/libzip) | `1.11.4` | BSD-3-Clause |
| [OpenSSL](https://www.openssl.org/) | `1.1.1w` | OpenSSL and SSLeay licenses |
| [stduuid](https://github.com/mariusbancila/stduuid) | `1.2.3` | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | `0.8.3` | BSD-2-Clause |
| [FFmpeg](https://ffmpeg.org/) | `8.1.2` (static link, GPL) | GPL-2.0-or-later |
| [x264](https://code.videolan.org/videolan/x264) | git commit (see `third_party/ffmpeg/ffmpeg-build-manifest.json`) | GPL-2.0-or-later |
| [x265](https://github.com/videolan/x265) | `3.6` | GPL-2.0-or-later |
| [libvpx](https://github.com/webmproject/libvpx) | `v1.15.0` | BSD-3-Clause |
| [libopus](https://github.com/xiph/opus) | `v1.5.2` | BSD-3-Clause |
| [zlib](https://github.com/madler/zlib) | `v1.3.1` | Zlib |

The complete Dear ImGui license text is distributed in `licenses/DearImGui-LICENSE.txt`. Other dependencies are resolved by xmake from their upstream packages; consult each linked project for its complete license text and source code.

Playback does not redistribute `LeviLamina.dll` or LeviMC's closed-source components. The LeviLamina repository provides `COPYING` and `COPYING.LESSER` for its LGPL-3.0 portions, while `EULA.en.md` and `EULA.zh.md` cover LeviMC closed-source software such as PreLoader and PeEditor.

The SymbolProvider repository contains no `LICENSE`, `COPYING`, `NOTICE`, or `DISCLAIMER.PD` file, and its xmake-repo package recipe does not declare a license. Its sole source file states that it has no assigned copyright and is placed in the Public Domain, while referring to a `DISCLAIMER.PD` file that is not present in the repository. This notice records the upstream state and is not a substitute for legal advice.

## FFmpeg (static link, GPL)

The export pipeline links FFmpeg statically under the GPL. When distributing builds that include this module, the following obligations apply:

1. **License texts**: `licenses/ffmpeg/` contains the license texts of FFmpeg (GPL), x264 (GPL), x265 (GPL), libvpx (BSD-3-Clause), libopus (BSD-3-Clause) and zlib (Zlib). They are copied into the distribution by the build script.
2. **Corresponding source**: complete corresponding source and rebuild instructions are provided by `scripts/build-ffmpeg/` (build scripts, pinned versions in `versions.txt`) and `third_party/ffmpeg/ffmpeg-build-manifest.json` (pinned commits, configure flags, toolchain). The `third_party/ffmpeg-src/` checkout retains the exact source used.
3. **Notice**: UI/export surfaces and release notes must state "built-in FFmpeg (GPL)".
