# 构建 Playback

[返回主文档](../README_ZH.md) | [English](building.md)

## 环境要求

- 带有 MSVC C++ 工具链的 Visual Studio 2022
- [xmake](https://xmake.io/)
- Git

Playback 目前面向 Windows x64 的 LeviLamina 客户端运行环境。`xmake.lua` 声明的依赖版本必须与目标 Minecraft 和 LeviLamina 发行分支保持一致。

## Release 构建

配置前请先检出与目标运行时匹配的分支：

| 源码分支 | Minecraft / LeviLamina | Playback 版本 |
| --- | --- | --- |
| `main`、`feat/26.10` | `26.10.*` | `0.4.1-mc26.10` |
| `develop`、`feat/26.20` | `26.20.*` | `0.4.1-mc26.20` |
| `feat/26.40` | `>=26.40.5 <26.50.0` | `0.4.1-mc26.40` |
| `feat/26.50` | `26.51.*` | `0.4.1-mc26.50` |
| `feat/26.60` | Minecraft `1.26.60.5`、LeviLamina `26.60.*` | `0.4.1-mc26.60` |

五条发行线均为预发布版本。设置 `PLAYBACK_VERSION` 只会改变打包版本标识，不会切换源码分支或 SDK 目标。`mc26.50` 后缀对应游戏 26.51；26.60 构建仅支持表中指定的游戏构建。

打包后的模组版本默认来自最近的 Git 发行标签。在标签创建前验证发行构建时，请显式设置 `PLAYBACK_VERSION`。

发布前请同步以下声明：

- `tooth.json` 中的 `version`。
- `CHANGELOG.md` 顶部的当前版本条目和比较链接。
- 中英文文档中的发行徽章、兼容性表、安装命令和兼容性说明。
- 仅当资源包本身发生语义变化时，才更新 `resources/manifest.json` 中 header 与 module 的版本数组。

Tooth `format_version`、资源包 `format_version` 和 VS Code 配置版本等外部 schema 字段不是 Playback 发行版本，不应随版本号一起修改。

在仓库根目录配置并执行干净的 Release 客户端构建。下例使用 26.20 线，请按当前检出的分支选择上表中的版本：

```powershell
$env:PLAYBACK_VERSION = 'v0.4.1-mc26.20'
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

打包后的模组位于 `bin/playback/`。原生界面翻译会复制到 `bin/playback/lang/`，图标字体会复制到 `bin/playback/fonts/`，轻量主菜单按钮资源包会安装到 `bin/playback/resource_packs/playback-ui/`。同一按钮资源包还会生成为 `bin/playback-ui.mcpack`，供单独手动导入；原生回放浏览器不在该资源包内。

Xmake 会使用 x264 构建固定版本的 FFmpeg 7.1 命令行运行时，并将静态可执行文件复制到 `bin/playback/tools/ffmpeg.exe`，提供 H.264 视频及 AAC、Opus、FLAC 音频编码。发行版用户无需单独安装 FFmpeg。首次源码构建需要下载并编译这套工具链，因此依赖配置会比后续命中缓存的构建耗时更长。

构建后请确认 `bin/playback/manifest.json` 与所选版本一致（本例为 `0.4.1-mc26.20`），确认 `bin/playback/tools/ffmpeg.exe` 存在，并运行 `git diff --check`。运行时改动应在各目标版本重新录制，并以原版标准／精美画质导出简短的 30 FPS MP4 和 PNG 片段。启用音频时检查音频完整性，并验证改动的选项；特定渲染路径的改动还需在对应画质下测试。

Release 工作流以标签源码构建，将标签传给 `PLAYBACK_VERSION`，从 `CHANGELOG.md` 提取对应版本说明，并上传 `Playback-client-windows-x64.zip` 和 `playback-ui.mcpack`。每个标签应指向匹配的版本分支。本地整理提交和标签不会发布内容，发布 GitHub Release 才会触发工作流。

## 刷新依赖

如果 prelink 报告无法找到 `bedrock_runtime_data`，请刷新包配置并重新构建：

```powershell
xmake repo -u
xmake f -c -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

提交修改前，请遵循 [CONTRIBUTING.md](../CONTRIBUTING.md) 中的格式化和验证要求。
