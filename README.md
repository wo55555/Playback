# Playback

Playback 是一个基于 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 的 Minecraft 基岩版客户端原生模组，目标是为基岩版客户端提供游戏录制、导出与回放能力。其设计参考了 Java 版 [Flashback](https://github.com/Moulberry/Flashback) 模组的架构理念。

## 特性

- **录制区块**：通过网络层 Hook 截获 `LevelChunkPacket`，缓存区块数据，按 tick 写入初始快照、分片和区块缓存文件。
- **异步保存**：使用后台写入线程生成录制分片、元数据和区块缓存文件，降低录制过程阻塞。
- **回放导出**：已接入通过 `libzip` 将录制目录导出为 `.playback` 压缩回放文件的流程，导出与打包结果仍待实机验证。
- **回放系统**：支持回放会话生命周期、自动检测回放文件和世界就绪后的初始快照入口。
- **主菜单回放入口**：在客户端主菜单注入回放按钮，通过原生 UI 事件打开回放列表并进入选中的回放文件。
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

如果 prelink 阶段报 `Cannot find bedrock_runtime_data`，通常是 xmake 的依赖路径缓存没有刷新；先执行 `xmake clean`，再重新执行 `xmake` 即可。

客户端 UI 资源会输出为：

- `bin/playback/playback-ui.mcpack`：可导入到 Minecraft 客户端的 UI 资源包。

也可以单独重新打包 UI 资源：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package_resource_pack.ps1 resources bin\playback\playback-ui.mcpack
```

如果使用 LeviLauncher 的独立实例，需要把资源包安装到该实例自己的 `Minecraft Bedrock/Users/Shared/games/com.mojang` 目录，并同步全局资源包版本：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install_resource_pack.ps1 resources "<LeviLauncher实例>\Minecraft Bedrock\Users\Shared\games\com.mojang" -UpdateGlobalResources
```

## 命令

### `playback version`

显示模组版本信息。

### `record start / pause / stop`

控制录制流程：

- `record start` — 开始或恢复录制，记录当前世界的区块快照与后续区块变化
- `record pause` — 暂停录制
- `record stop`  — 结束录制

### `replay start <filename>`

加载回放文件并启动回放。当前回放会话会在数据目录的 `replays/` 下按世界 ID 自动检测 `<levelId>.playback` 文件。

### `replay play / pause`

回放进入世界时默认处于暂停状态；暂停只停止时间线推进，区块仍会继续加载。等待初始区块完成后，使用 `replay play` 开始播放，使用 `replay pause` 冻结时间线。

> 配置中的 `replay.enabled` 只控制 `replay start` 子命令；`play` / `pause` 控制始终可用。命令名称（`record` / `replay`）可在配置中自定义。

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
├── ui/
│   ├── MainMenuHooks.cpp/h     # 主菜单按钮事件绑定与回放列表弹窗
│   └── ReplayBrowser.cpp/h     # 回放文件扫描、排序、过滤和打开入口
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

## UI 修改范式

这个项目的客户端 UI 接入分成四层：

1. 资源层：把 Bedrock JSON UI 放到 `resources/ui/`，通过 `resources/ui/_ui_defs.json` 加载增量文件。覆盖原生界面时优先沿用原生文件名，例如主菜单使用 `resources/ui/start_screen.json`，再用 `modifications` 插入按钮，并把按下事件命名为 `button.playback_open_replays`。
2. 构建层：`xmake.lua` 的 `after_build` 会将 `resources/` 打包为 `playback-ui.mcpack`，因为 `levibuildscript/modpacker` 默认只复制 DLL、PDB 和模组 manifest。
3. Controller 层：在 `StartMenuScreenController::_registerBindings` 之后注册同名按钮事件，使用 `_getNameId("button.playback_open_replays")` 和 `registerButtonPressedHandler(...)` 把 JSON UI 事件连接到 C++ 逻辑。
4. 业务层：把回放扫描、排序、过滤和打开封装在 `ui/ReplayBrowser`，按钮处理只负责刷新列表并展示入口 UI，最终调用 `ReplaySession::start(path)`。

继续添加原生 UI 按钮时，优先沿用同一套路：先约定稳定的 button id，再写资源 JSON 显示控件，随后在对应 ScreenController 的 `_registerBindings` hook 中注册 handler，最后把真正业务逻辑放到独立模块里。资源包变更后要递增 `resources/manifest.json` 的 header/module 版本，否则 Minecraft 可能继续使用已导入的旧缓存。

## 当前进度

| 模块      | 状态     | 说明                                                               |
| --------- | -------- | ------------------------------------------------------------------ |
| 命令系统  | ✅ 完成   | playback / record / replay 命令                                    |
| 网络 Hook | ✅ 完成   | LevelChunkPacket 拦截与区块缓存                                    |
| 录制引擎  | ✅ 已实现 | 已支持录制区块快照、tick 分片、暂停恢复和元数据写入                |
| I/O 层    | 🚧 待验证 | ReplayWriter / ReplayReader / 异步保存器已接入，完整写入链路待实测 |
| 导出系统  | 🚧 待验证 | 已接入 metadata、分片和区块缓存打包到 zip / `.playback` 的流程     |
| 回放引擎  | 🚧 进行中 | 会话生命周期与世界就绪检测已接入，调度待完善                       |
| 动作系统  | 🚧 进行中 | 已实现基础动作与动作注册，更多回放动作待补齐                       |

## 已知待办

- 导出 `.playback` 文件与 zip 打包内容仍需实机验证。
- 打包产物的 metadata、录制分片和区块缓存文件完整性仍需校验。
- 回放文件读取与 `.playback` 解包流程仍需接入 `ReplaySession`。
- `ActionLevelChunkCached` 的区块恢复逻辑仍待实现。
- 回放时间轴调度仍需完善。

## 许可证

CC0-1.0 © LeviMC(LiteLDev)
