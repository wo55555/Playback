# 构建 Playback

[返回主文档](../README_ZH.md) | [English](building.md)

## 环境要求

- 带有 MSVC C++ 工具链的 Visual Studio 2022
- [xmake](https://xmake.io/)
- Git

Playback 目前面向 Windows x64 的 LeviLamina 客户端运行环境。`xmake.lua` 声明的依赖版本必须与目标 Minecraft 和 LeviLamina 发行分支保持一致。

## Release 构建

执行发行构建前，请同步以下版本声明：

- `xmake.lua` 中的 `mod_version`。
- `tooth.json` 中的 `version`。
- `resources/manifest.json` 中资源包 header 与 module 的版本数组。
- `CHANGELOG.md` 顶部的当前版本条目和比较链接。

Tooth `format_version`、资源包 `format_version` 和 VS Code 配置版本等外部 schema 字段不是 Playback 发行版本，不应随版本号一起修改。

在仓库根目录配置并执行干净的 Release 客户端构建：

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

打包后的模组位于 `bin/playback/`。原生界面翻译会复制到 `bin/playback/lang/`，图标字体会复制到 `bin/playback/fonts/`，轻量主菜单按钮资源包会安装到 `bin/playback/resource_packs/playback-ui/`。同一按钮资源包还会生成为 `bin/playback-ui.mcpack`，供单独手动导入；原生回放浏览器不在该资源包内。

Xmake 会使用 x264 构建固定版本的 FFmpeg 7.1 命令行运行时，并将静态可执行文件复制到 `bin/playback/tools/ffmpeg.exe`。发行版用户无需单独安装 FFmpeg。首次源码构建需要下载并编译这套工具链，因此依赖配置会比后续命中缓存的构建耗时更长。

构建完成后，请确认 `bin/playback/manifest.json` 显示 `0.2.0-mc26.10`、`bin/playback/tools/ffmpeg.exe` 存在，并执行 `git diff --check`。涉及运行时行为的发行版本还应在支持的渲染路径上分别导出短 PNG 序列和 MP4。

## 刷新依赖

如果 prelink 报告无法找到 `bedrock_runtime_data`，请刷新包配置并重新构建：

```powershell
xmake repo -u
xmake f -c -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

提交修改前，请遵循 [CONTRIBUTING.md](../CONTRIBUTING.md) 中的格式化和验证要求。
