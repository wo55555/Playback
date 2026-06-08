# Playback

Playback 是一个基于 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 的 Minecraft 基岩版客户端原生模组，提供游戏录制与回放能力。其设计参考了 Java 版 [Flashback](https://github.com/Moulberry/Flashback) 模组的架构理念。

## 特性

- **录制系统**：通过网络层 Hook 截获 `LevelChunkPacket` 等数据包，缓存区块数据并生成录制文件。
- **回放系统**：加载录制文件，在临时世界中按时间轴回放游戏状态与网络包。
- **动作系统**：基于 Action 抽象的回放动作框架（`ActionNextTick`、快照等）。
- **可配置命令**：支持通过配置文件启用/禁用录制与回放命令，自定义命令名称。

## 快速开始

### 环境要求

- Windows x64
- Visual Studio 2022（MSVC，C++20）
- [xmake](https://xmake.io/) 构建工具
- [LeviLamina](https://github.com/LiteLDev/LeviLamina) 开发环境

### 构建

```bash
xmake f -y -p windows -a x64 -m release
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

加载 `data/records/<filename>` 下的录制文件并启动回放。

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
    │   └── Action.cpp/h        # 回放动作抽象（ActionNextTick 等）
    ├── io/
    │   ├── AsyncReplaySaver.h  # 异步写入器（骨架）
    │   └── ReplayWriter.h      # 二进制写入器（骨架）
    ├── record/
    │   ├── Recorder.cpp/h      # 录制引擎（区块缓存、快照、元数据）
    │   └── NetworkHooks.cpp    # 网络层 Hook（LevelChunkPacket 拦截）
    └── replay/
        └── ReplaySession.cpp/h # 回放会话管理（加载、世界就绪、时间轴）
```

## 当前进度

| 模块      | 状态     | 说明                                       |
| --------- | -------- | ------------------------------------------ |
| 命令系统  | ✅ 完成   | playback / record / replay 命令            |
| 网络 Hook | ✅ 完成   | LevelChunkPacket 拦截与缓存                |
| 录制引擎  | 🚧 进行中 | 区块缓存、元数据序列化；数据持久化待完善   |
| 回放引擎  | 🚧 进行中 | 会话生命周期、世界就绪检测；回放调度待完善 |
| 动作系统  | 🏗 骨架   | Action 抽象已定义，具体动作待实现          |
| I/O 层    | 🏗 骨架   | ReplayWriter / AsyncReplaySaver 占位类     |

## 许可证

CC0-1.0 © LeviMC(LiteLDev)
