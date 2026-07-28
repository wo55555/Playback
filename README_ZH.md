# Playback

[![Discord](https://img.shields.io/badge/Discord-Playback-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/mUhRUD8AM)
[![QQ](https://img.shields.io/badge/QQ-Playback-EB1923?style=for-the-badge&logo=qq&logoColor=white)](https://qm.qq.com/q/ufJatMDcha)

[![English](https://img.shields.io/badge/English-informational?style=for-the-badge)](README.md)
![简体中文](https://img.shields.io/badge/简体中文-inactive?style=for-the-badge)

Playback 是一个基于 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 的 Minecraft 基岩版客户端原生模组，用于录制、导出和回放游戏过程。回放架构参考了 Java 版 [Flashback](https://github.com/Moulberry/Flashback) 模组，并针对基岩版客户端生命周期进行了适配。

> [!WARNING]
> `0.1.0-alpha.1` 是第一个公开测试版本。请备份重要世界和录制文件；在 Minecraft、LeviLamina 或 Playback 版本发生变化后，不保证旧回放仍然兼容。

## 功能

- 捕获已加载区块、方块实体、实体移动、玩家状态、时间和经过筛选的客户端安全数据包。
- 异步写入回放快照和时间线数据，减少录制过程中的卡顿。
- 将录制结果导出为便于转移的回放压缩包。
- 通过原生主菜单回放浏览器，在隔离的本地回放世界中打开回放。
- 提供游戏内时间线，支持播放、暂停、跳转、倍速调整和退出回放。
- 为命令、回放编辑器和资源包 UI 提供英文及简体中文本地化。

## 兼容性

- Windows 基岩版
- LeviLamina 客户端 `26.10.*`

> [!TIP]
> Playback 为纯客户端模组，支持客户端与服务端录制。

## 快速开始

### 安装发布版本

1. 从 GitHub Release 下载 `Playback-client-windows-x64.zip` 和 `playback-ui.mcpack`。
2. 将压缩包内的 `playback` 目录解压到 LeviLamina 实例的 `mods` 目录。
3. 打开单独下载的 `playback-ui.mcpack`，将 UI 资源包导入 Minecraft。
4. 在 Minecraft 中进入 **设置 > 全局资源 > 我的资源包**，激活 **Playback UI**，然后重启客户端。

> [!IMPORTANT]
> 必须安装并激活 `playback-ui.mcpack` 资源包，否则主菜单不会显示 Playback 回放按钮。

完成后，主菜单中应显示 Playback 按钮。

### 录制

进入世界后，打开客户端命令控制台并使用：

```text
record start
record pause
record stop
```

`record start` 开始或继续录制，`record pause` 暂停录制，`record stop` 结束录制并导出回放。导出的 `.zip` 文件位于 Playback 的 `data/replays` 目录。

### 回放

1. 返回主菜单并选择 **Playback**。
2. 在回放浏览器中选择 `.playback` 或兼容的 `.zip` 回放文件。
3. 等待隔离回放世界和初始区块加载完成。
4. 使用底部时间线播放、暂停、跳转、调整倍速或跳至时间线两端；使用 **File > Exit Replay** 退出回放。

## 从源码构建

环境要求：

- 带有 MSVC C++ 工具链的 Visual Studio 2022
- [xmake](https://xmake.io/)
- Git

配置并执行干净的 Release 客户端构建：

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

打包后的模组位于 `bin/playback/`，命令和回放编辑器的翻译文件位于 `bin/playback/lang/`。构建过程还会从 `resources/` 生成 `bin/playback/playback-ui.mcpack`，其中包含主菜单和回放浏览器 UI 的本地化资源。

如果 prelink 报告无法找到 `bedrock_runtime_data`，请刷新包配置并重新构建：

```powershell
xmake repo -u
xmake f -c -y -p windows -a x64 -m release --target_type=client
xmake -r -y
```

## 命令

| 命令               | 说明                           |
| ------------------ | ------------------------------ |
| `playback version` | 显示当前加载的 Playback 版本。 |
| `record start`     | 开始或继续录制当前世界。       |
| `record pause`     | 暂停当前录制。                 |
| `record stop`      | 停止录制并导出回放。           |

## 语言

Playback 目前提供英文（`en_US`）和简体中文（`zh_CN`）翻译。命令与回放编辑器的翻译文件位于 `src/lang/`，资源包 UI 的翻译文件位于 `resources/texts/`。

## 开发状态与计划

- 录制、导出和回放 GUI 正在持续构建与优化中。
- 后续将重点调试多人服务器会话的录制与回放，欢迎测试并反馈问题。
- 计划开发摄影机运动、视频渲染与导出等功能。

## 已知限制

- 回放格式仍在开发中，Alpha 版本之间可能发生变化。
- Playback 重建的是已录制的客户端可见状态，并不是原始服务器模拟过程的确定性副本。
- 当前不会将待执行计划刻和村庄、袭击、POI 等服务端系统保存为权威模拟状态。
- Minecraft 或 LeviLamina 更新后，需要重新确认兼容性。

报告可复现问题时，请尽量附带日志、相关版本和最小回放文件。

## 参与贡献

构建、格式化和 Pull Request 流程见 [CONTRIBUTING.md](CONTRIBUTING.md)。安全问题请按照 [SECURITY.md](SECURITY.md) 私下报告，不要创建公开 Issue。

## 致谢

特别感谢 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 的维护者与社区提供原生模组开发平台和工具，使 Playback 得以实现；同时感谢 [Flashback](https://github.com/Moulberry/Flashback) 项目及其贡献者，其回放理念与架构为 Playback 提供了重要启发。

## 许可证

Playback 采用 [CC0-1.0](LICENSE) 发布。第三方组件保留各自许可证，详情见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和 `licenses/` 目录。
