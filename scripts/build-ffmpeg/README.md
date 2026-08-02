# 内置静态 FFmpeg 构建（步骤 1）

Playback 的导出链路以 **GPL 静态链接**方式内置 FFmpeg（libav API），不再依赖外部 `ffmpeg.exe` 或用户手动配置。

本文描述如何在本机构建可复现的静态 FFmpeg 8.1.2 软编全量构建，并随发行物履行 GPL 义务。

## 一、产物与目录

| 目录/文件 | 说明 | 是否入库 |
|---|---|---|
| `third_party/ffmpeg/` | 构建产物：`include/` + `lib/` + `ffmpeg-build-manifest.json` | 否（.gitignore） |
| `third_party/ffmpeg-src/` | 下载/检出的各库源码（构建后保留，用于审计与对应源码） | 否 |
| `licenses/ffmpeg/` | 各库许可证文本（由脚本拷贝） | 是 |
| `scripts/build-ffmpeg/` | 本目录：版本清单、入口脚本、MSYS2 构建脚本、本文档 | 是 |

## 二、前置条件（一次性）

1. **MSYS2**：安装自 <https://www.msys2.org/>（默认 `C:\msys64`）。首次运行后执行：

   ```bash
   pacman -Syu   # 完成后 MSYS2 会自动关闭，重新打开终端
   pacman -Su
   pacman -S --needed git make nasm pkg-config cmake
   ```

2. **Visual Studio 2022**（C++ 桌面工作负载），脚本通过 vswhere 自动定位 `vcvars64.bat`。

3. 构建时保持网络可用（需要下载 FFmpeg 8.1.2 tarball 与各库源码）。

## 三、构建

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-ffmpeg/build-ffmpeg.ps1
```

脚本执行：

1. 定位 MSYS2 与 VS；
2. 按 `versions.txt` 固定版本下载/检出源码（FFmpeg 8.1.2、x264、x265 3.6、libvpx v1.15.0、libopus v1.5.2、zlib v1.3.1）；
3. 在 VS 环境 + MSYS2 bash 中依次构建依赖（`build-deps.sh`）与 FFmpeg（`build-ffmpeg.sh`）；
4. 收集 `third_party/ffmpeg` 产物并生成 `ffmpeg-build-manifest.json`；
5. 拷贝各库许可证到 `licenses/ffmpeg/`。

构建完成后启用链接：

```powershell
xmake f --playback_ffmpeg=y
xmake build playback
```

### 常用参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `-FfmpegVersion` | `8.1.2` | FFmpeg 版本（同步更新 `versions.txt`） |
| `-X264Commit` | 空（远端 HEAD） | 固定 x264 commit 后脚本会检出该 commit |
| `-X265Tag` / `-LibvpxTag` / `-LibopusTag` / `-ZlibTag` | 见上 | 固定 tag |
| `-VsTarget` | `vs17` | libvpx 目标；VS2019 用 `vs16` |
| `-Jobs` | 逻辑核数 | 并行度 |
| `-SkipDownload` | 关 | 跳过下载，仅校验已有源码并构建 |

### 可复现性

- 源码目录保留（`third_party/ffmpeg-src/`），x264/libvpx/libopus/zlib 的 git commit 与 FFmpeg 版本写入 `ffmpeg-build-manifest.json`。
- 生产发布前建议：固定 `-X264Commit`；为 FFmpeg tarball 校验 SHA-256（可在 `build-ffmpeg.ps1` 的 `Get-Source` 调用中传入 `-ExpectedSha256`）。

## 四、GPL 合规与对应源码

本模块采用 **GPL 静态链接**，启用 `libx264`、`libx265`（GPL-2.0-or-later），整体以 GPL 兼容方式发布。必须遵守：

1. 随发行物提供 `licenses/ffmpeg/` 下全部许可证文本；
2. 提供完整对应源代码及获取/重建说明：
   - Playback 自身源码；
   - FFmpeg 8.1.2 源码与精确 configure 参数（见 manifest）；
   - x264 / x265 / libvpx / libopus / zlib 源码及其固定 commit/tag（见 manifest 与 `versions.txt`）；
   - 构建脚本 `scripts/build-ffmpeg/`；
3. 发布前核对 `ffmpeg-build-manifest.json`（版本、commit、configure、编译器、时间），并更新 `THIRD_PARTY_NOTICES.md`；
4. 设置页/导出页/发行说明标注“内置 FFmpeg（GPL）”，不得写成外部工具或 LGPL-only 方案。

## 五、更新 FFmpeg 版本

1. 修改 `versions.txt` 与 `build-ffmpeg.ps1` 默认参数；
2. 删除 `third_party/ffmpeg-src/` 中对应目录后重新构建；
3. 重新生成 manifest 与许可证，更新 `THIRD_PARTY_NOTICES.md`；
4. 如 ABI/API 变化，同步调整 `FfmpegBuildInfo` 与 `LibavEncoder`/`LibavMuxer` 调用。

## 六、故障排查

| 现象 | 处理 |
|---|---|
| `cl.exe not found` | 脚本未从 VS 环境启动 MSYS2；确认 vswhere 能找到 VS |
| `link.exe` 冲突 | `build-deps.sh` 会自动临时移开 MSYS2 的 `/usr/bin/link.exe` |
| nasm 缺失 | `pacman -S nasm` |
| x265 库名不符 | 检查 `$PREFIX/lib` 下实际库名，调整 `build-deps.sh` 的统一拷贝逻辑 |
| libvpx target 报错 | 换 `-VsTarget vs16`（对应 VS2019），或升级 libvpx |
| configure 检测不到某库 | 确认 `$PREFIX/include`、`$PREFIX/lib` 有对应头与库；库名与 `--extra-libs` 一致 |
