# Playback 安装与使用

[返回主文档](../README_ZH.md) | [English](getting-started.md)

## 安装前须知

Playback 是面向 Windows x64 的 LeviLamina 纯客户端模组。安装时必须选择与实例中的 Minecraft 和 LeviLamina 版本匹配的发行版本。

| Minecraft / LeviLamina | Playback 版本                                                                       |
| ---------------------- | ----------------------------------------------------------------------------------- |
| `26.10.*`              | [`v0.2.0-mc26.10`](https://github.com/wo55555/Playback/releases/tag/v0.2.0-mc26.10) |
| `26.20.*`              | [`v0.1.1-mc26.20`](https://github.com/wo55555/Playback/releases/tag/v0.1.1-mc26.20) |

> [!IMPORTANT]
> 建议尽量使用未安装其他第三方模组的独立 LeviLamina 实例。目前暂不保证与其他模组广泛兼容。

> [!CAUTION]
> `v0.2.0-mc26.10` 仍是测试版本。旧版 Playback 创建的回放不兼容，必须重新录制。受影响服务器在 2026 年 8 月 20 日热更新前录制的回放可能已经缺少可移植区块或自定义实体注册数据，这类归档无法修复，也必须重新录制；数据完整的 `v0.2.0-mc26.10` 回放无需转换。配置版本和录制文件的快照上下文版本均保持为 `1`，不提供迁移。

## 使用 LeviLauncher 和 Lip 安装

以下截图以 `26.10` 实例为例，仅作安装流程示意。使用 `26.20` 时，请选择相互匹配的 Minecraft、LeviLamina 和旧版 Playback 发行线。

1. 在左侧边栏选择 **Download（下载）**，找到需要的 Minecraft 版本，通过安装菜单创建使用 **LeviLamina** 加载器的实例。

<p align="center">
  <img src="images/quick-start/zh-cn/install-instance.webp" alt="打开 Download 页面并安装使用 LeviLamina 的 Minecraft 实例" width="900">
</p>

2. 选择 **Instances（实例）**，打开新实例的设置，在**加载器**页面确认已经安装对应版本的 LeviLamina。

<p align="center">
  <img src="images/quick-start/zh-cn/verify-levilamina.webp" alt="打开 Instances 页面并确认实例已经安装 LeviLamina 加载器" width="900">
</p>

3. 选择 **Launch（启动）**返回主页面，选中目标实例，然后在**内容下载**区域选择 **lip**。

<p align="center">
  <img src="images/quick-start/zh-cn/open-lip.webp" alt="打开 Launch 主页面并在内容下载中选择 lip" width="900">
</p>

4. 搜索 **Playback**，然后打开由 `wo55555` 发布的软件包。

<p align="center">
  <img src="images/quick-start/zh-cn/search-playback.webp" alt="在 lip 中搜索 Playback" width="900">
</p>

5. 手动选择 **LL 依赖**和**游戏版本**与当前实例一致的发行版本，然后点击该版本所在行的**安装**。Lip 不会根据已安装的 LeviLamina 版本自动选择 Playback 版本。

<p align="center">
  <img src="images/quick-start/zh-cn/install-playback.webp" alt="通过 lip 安装兼容的 Playback 版本" width="900">
</p>

安装完成后启动或重启游戏，内置按钮资源包会自动加载，Minecraft 主菜单中应显示 **Playback** 按钮。Release 同时提供 `playback-ui.mcpack` 供单独手动导入。

## 使用 Lip 命令行安装

在目标 LeviLamina 实例的根目录中执行对应命令：

```powershell
# Minecraft / LeviLamina 26.10
lip install github.com/wo55555/Playback@0.2.0-mc26.10#client

# Minecraft / LeviLamina 26.20
lip install github.com/wo55555/Playback@0.1.1-mc26.20#client
```

> [!NOTE]
> 安装客户端变体时必须保留末尾的 `#client`。请始终明确指定并核对匹配的版本；Lip 不会根据已安装的 LeviLamina 版本自动选择。

## 手动安装

从匹配的发行版本下载 `Playback-client-windows-x64.zip`，将压缩包内的 `playback` 目录解压到 LeviLamina 实例的 `mods` 目录，然后重启客户端。

## 录制

进入世界后，打开客户端命令控制台并使用：

```text
record start
record pause
record stop
```

`record start` 开始或继续录制，`record pause` 暂停录制，`record stop` 结束录制并导出回放。导出的回放归档位于 Playback 的 `data/replays` 目录。

## 回放

1. 返回主菜单并选择 **Playback**。
2. 在回放浏览器中选择 `.playback` 或兼容的 `.zip` 回放文件。
3. 等待隔离回放世界和初始区块加载完成。
4. 使用时间线播放、暂停、跳转、调整倍速或跳至两端；使用 **File > Exit Replay** 退出回放。

## 相机编辑

1. 在时间线编辑器中添加或选择相机。
2. 将播放头移动到目标 tick，使用**在播放头添加关键帧**。
3. 在属性检查器中编辑位置、偏航、俯仰、滚转、FOV、插值模式和三次贝塞尔控制点。
4. 添加更多关键帧并播放回放以预览相机插值；暂停时会恢复为可自由移动的观察者相机。

每次已录制的维度变化都会自动切断相机插值。不同维度片段中的关键帧不会相互连接，即使中间维度没有相机关键帧也一样。

## 实验性视频导出

在回放编辑器中打开**文件 > 导出…**，可配置：

- 输出目录和文件名。
- MP4 视频或 PNG 图像序列。
- 开始和结束 tick。
- 帧率与输出分辨率。
- SSAA 与预热帧。

实验性导出器根据回放时间逐帧渲染，而不是实时录屏。MP4 使用模组内置的 FFmpeg/libx264；如果 FFmpeg 无法启动，仍可使用 PNG 序列。默认输出目录为 `mods/playback/exports`。

| 渲染后端 | 格式                | 稳定 SSAA |
| -------- | ------------------- | --------- |
| D3D12    | H.264 MP4、PNG 序列 | 1x、2x    |
| D3D11    | H.264 MP4、PNG 序列 | 1x        |

> [!IMPORTANT]
> 视频导出仍为实验性功能，当前不包含音频。相机区域必须真实存在于已录制的回放数据中，Playback 无法重建从未录制的区块。跨维度加载、回放状态准备、UI 稳定和导出预热均有超时保护。Playback 不再要求固定相机邻域内的每个区块都存在，但回放数据缺失时仍可能无法渲染预期场景。

开发说明见[源码构建](building.zh-CN.md)，发行历史见[更新日志](../CHANGELOG.md)。
