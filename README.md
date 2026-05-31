# Playback

Playback 是一个基于 LeviLamina 的客户端原生模组，目标是为 Minecraft 客户端提供录制与回放能力的实验性实现。

当前仓库已经包含命令入口、网络钩子、录制控制框架和回放相关的代码骨架，适合作为后续完善网络包记录、状态快照和回放调度的起点。

## 特性

- `playback version`：查看模组版本信息。
- `record start / pause / stop`：控制录制流程。
- 客户端网络层钩子：为后续的数据记录与回放提供入口。
- 回放模块骨架：预留了后续恢复状态与执行回放的扩展点。

## 快速开始

1. 安装并配置 LeviLamina 开发环境。
2. 在项目根目录执行：

```bash
xmake f -y -p windows -a x64 -m release
xmake
```

3. 构建完成后，产物会输出到 `bin/` 目录。

## 命令

```bash
playback version
```

输出模组版本信息。

```bash
record start
```

开始录制。

```bash
record pause
```

暂停录制。

```bash
record stop
```

结束录制。

> 默认命令名为 `record`，可在配置中调整。

## 项目状态

该项目目前仍处于早期开发阶段，已经具备基础框架，但录制数据持久化、回放调度和完整的数据解析逻辑仍在逐步完善中。

## 许可证

CC0-1.0 © LeviMC(LiteLDev)
