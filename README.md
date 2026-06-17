# Playback

Playback 是一个基于 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 的 Minecraft 基岩版客户端原生模组，目标是为基岩版客户端提供游戏录制、导出与回放能力。其设计参考了 Java 版 [Flashback](https://github.com/Moulberry/Flashback) 模组的架构理念。

## 特性

- **录制系统**：通过网络层 Hook 截获 `LevelChunkPacket`，缓存区块数据并写入录制快照。
- **异步保存**：使用后台写入线程生成录制分片、元数据和区块缓存文件，降低录制过程阻塞。
- **回放导出**：通过 `libzip` 将录制目录导出为 `.playback` 压缩回放文件。
- **回放系统**：支持回放会话生命周期、自动检测回放文件和世界就绪后的初始快照入口。
- **动作系统**：基于 Action 抽象的回放动作框架，已包含 `ActionNextTick` 与 `ActionLevelChunkCached`。
- **可配置命令**：支持通过配置文件启用/禁用录制与回放命令，自定义命令名称。

## 快速开始

### 环境要求

- Windows x64
- Visual Studio 2022（MSVC，C++20）
- [xmake](https://xmake.io/) 构建工具
- [LeviLamina](https://github.com/LiteLDev/LeviLamina) 开发环境
- 依赖库：`stduuid`、`xxhash`、`openssl`、`libzip`（由 xmake 拉取）

### 构建

```bash
xmake f -y -p windows -a x64 -m release --target_type=client
xmake
```

构建产物输出到 `bin/` 目录。

## 命令

### `playback version`

显示模组版本信息。

### `record start / pause / stop`

控制录制流程：

- `record start` — 开始录制，激活网络层 Hook 并记录游戏状态
- `record pause` — 暂停录制
- `record stop`  — 结束录制

### `replay start <filename>`

加载回放文件并启动回放。当前回放会话会在数据目录的 `replays/` 下按世界 ID 自动检测 `<levelId>.playback` 文件。

> 命令名称（`record` / `replay`）可在配置中自定义，也可单独禁用。

## 项目结构

```
src/playback/
├── Playback.cpp/h              # 模组主入口，生命周期管理
├── Config.h                    # 配置结构（命令开关、语言等）
├── MemoryOperators.cpp         # 内存操作
├── command/
│   ├── Command.cpp/h           # playback 命令注册
│   ├── Record.cpp              # record 命令（start/pause/stop）
│   └── Replay.cpp              # replay 命令（start <filename>）
└── functions/
    ├── action/
    │   ├── Action.cpp/h        # 回放动作抽象（ActionNextTick、ActionLevelChunkCached）
    │   └── ActionRegistry.cpp  # 动作注册表
    ├── io/
    │   ├── AsyncReplaySaver.*  # 异步保存、分片写入、区块缓存文件写入
    │   ├── ReplayWriter.cpp    # 回放二进制写入器
    │   ├── ReplayReader.cpp    # 回放二进制读取器
    │   └── cache/              # 区块包缓存与去重结构
    ├── record/
    │   ├── Recorder.cpp/h      # 录制引擎（区块缓存、快照、元数据）
    │   ├── ReplayExporter.cpp  # 导出 .playback 压缩文件
    │   └── NetworkHooks.cpp    # 网络层 Hook（LevelChunkPacket 拦截）
    └── replay/
        └── ReplaySession.cpp/h # 回放会话管理（加载、世界就绪、时间轴）
```

## 当前进度

| 模块      | 状态     | 说明                                            |
| --------- | -------- | ----------------------------------------------- |
| 命令系统  | ✅ 完成   | playback / record / replay 命令                 |
| 网络 Hook | ✅ 完成   | LevelChunkPacket 拦截与区块缓存                 |
| 录制引擎  | 🚧 进行中 | 支持区块快照、元数据结构和停止录制导出流程      |
| I/O 层    | 🚧 进行中 | 已实现 ReplayWriter / ReplayReader / 异步保存器 |
| 导出系统  | 🚧 进行中 | 已支持 metadata、分片和区块缓存打包到 zip       |
| 回放引擎  | 🚧 进行中 | 会话生命周期与世界就绪检测已接入，调度待完善    |
| 动作系统  | 🚧 进行中 | 已实现基础动作与动作注册，更多回放动作待补齐    |

## 已知待办

- 回放文件读取与 `.playback` 解包流程仍需接入 `ReplaySession`。
- `ActionLevelChunkCached` 的区块恢复逻辑仍待实现。
- 录制分片元数据写入与回放时间轴调度仍需完善。

## 许可证

CC0-1.0 © LeviMC(LiteLDev)
