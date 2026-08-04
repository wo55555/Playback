# 构建 Playback

[返回主文档](../README_ZH.md) | [English](building.md)

## 环境要求

- 带有 MSVC C++ 工具链的 Visual Studio 2022
- [xmake](https://xmake.io/)
- Git

Playback 目前面向 Windows x64 的 LeviLamina 客户端运行环境。`xmake.lua` 声明的依赖版本必须与目标 Minecraft 和 LeviLamina 发行分支保持一致。

## Release 构建

在仓库根目录配置并执行干净的 Release 客户端构建：

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

打包后的模组位于 `bin/playback/`。原生界面翻译会复制到 `bin/playback/lang/`，图标字体会复制到 `bin/playback/fonts/`，轻量主菜单按钮资源包会安装到 `bin/playback/resource_packs/playback-ui/`。同一按钮资源包还会生成为 `bin/playback-ui.mcpack`，供单独手动导入；原生回放浏览器不在该资源包内。

## 刷新依赖

如果 prelink 报告无法找到 `bedrock_runtime_data`，请刷新包配置并重新构建：

```powershell
xmake repo -u
xmake f -c -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

提交修改前，请遵循 [CONTRIBUTING.md](../CONTRIBUTING.md) 中的格式化和验证要求。
