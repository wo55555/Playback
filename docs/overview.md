# Playback 快速解析（一页）

> 目标：用一张图看清全貌，用两张时序图看清录制与回放的关键路径。

---

## 1. 分层架构（模块关系总览）

```mermaid
flowchart TB
    subgraph ENTRY["入口层"]
        MOD["Playback (单例)"]
        CFG["PlaybackConfig"]
        LIFECYCLE["lifecycle<br/>load / enable / disable"]
    end

    subgraph CMD["命令层"]
        PCMD["playback version"]
        RCMD["record start/pause/stop"]
    end

    subgraph CORE["核心功能层 functions/"]
        REC["record/<br/>Recorder · NetworkHooks<br/>ChunkMutationBarrier<br/>AsyncReplaySaver"]
        REP["replay/<br/>ReplaySession · ReplayDriver<br/>ReplayWorldManager"]
        ACT["action/<br/>Action · ActionRegistry"]
        IO["io/<br/>ReplayWriter · ReplayReader<br/>CachedChunkPacket · Snappy"]
        TICK["tick/<br/>TickScheduler"]
    end

    subgraph EDITOR["编辑器层 editor/"]
        ECTX["context<br/>EditorContext · EditorState"]
        ECTL["controller<br/>时间轴 / 播放控制"]
        EREN["renderer<br/>D3D12 Hook · ImGui 渲染"]
        EUI["ui/<br/>Timeline / 面板"]
    end

    subgraph SCREEN["界面集成层 screen/"]
        MMH["main-menu-hooks<br/>主菜单按钮挂载"]
        RB["replay-browser<br/>回放列表 / 启动回放"]
    end

    subgraph UTIL["工具层 utils/"]
        PATH["PathUtils<br/>replays / temp 目录"]
        LHM["LinkedHashMap<br/>保序哈希表"]
    end

    subgraph RES["资源层 resources/"]
        UIPACK["ui-pack<br/>按钮 / 图标 资源包"]
    end

    MOD --> CFG
    MOD --> LIFECYCLE
    LIFECYCLE --> CMD
    LIFECYCLE --> EDITOR
    LIFECYCLE --> SCREEN

    CMD --> REC
    CMD --> REP

    REC -- "Action 事件流" --> ACT
    REC -- "异步落盘 + Snappy" --> IO
    ACT -- "注册 / 序列化 / 反序列化" --> IO
    REP -- "读取 Action" --> IO
    REP -- "Action.handle()" --> ACT
    REP --> TICK
    TICK --> REC

    EDITOR -- "查询 / 控制" --> REP
    EUI --> EREN
    ECTL --> ECTX
    EREN -- "D3D12 Present Hook" --> EUI

    SCREEN -- "打开回放浏览器" --> RB
    RB -- "创建 ReplaySession" --> REP

    REC --> PATH
    IO --> PATH
    LHM -. "REC / IO 内部容器" .- REC
    LHM -. "REC / IO 内部容器" .- IO
    MMH --> UIPACK
```

**关键关系一句话**

- `Playback` 是一切的总开关，初始化 `ActionRegistry`、挂 D3D12 Hook、注册命令、创建主菜单按钮。
- `Recorder` 只生产 `Action`；`ReplaySession` 只消费 `Action`；两者共享 `ActionRegistry` + `io` 层的二进制协议。
- `editor/` 永远不直接动世界数据，只读 `ReplaySession` 状态 + 调控制器。
- `utils/` 是被所有上层调用的基础服务，不反向依赖任何上层。

---

## 2. 录制时序（record flow）

```mermaid
sequenceDiagram
    autonumber
    participant U as 用户
    participant CMD as record 命令
    participant R as Recorder
    participant NH as NetworkHooks
    participant BAR as ChunkMutationBarrier
    participant AR as AsyncReplaySaver
    participant FS as 磁盘 (Snappy)

    U->>CMD: record start
    CMD->>R: begin()
    R->>AR: open(replays/{uuid})
    AR->>FS: 写文件头 + ActionRegistry 名表
    NH-->>R: onPacket(packetId, payload)
    R->>R: 写入 ActionNextTick (tick 边界)
    R->>BAR: waitUntilSafeToCapture()
    BAR-->>R: tick 切换瞬间解锁
    R->>AR: write(ChunkCached / SubChunkCached)
    AR->>FS: 后台线程：Snappy 压缩 + 落盘
    Note over AR,FS: CachedChunkPacket 去重同区块
    U->>CMD: record stop
    CMD->>R: end()
    R->>AR: close()
    AR->>FS: 写入 snapshot 块 + finalize zip
```

**要点**

- **tick 边界** 是唯一允许捕获区块快照的时刻（`ChunkMutationBarrier`）。
- **网络包** 由 `NetworkHooks` 旁路捕获，按原始 packetId 写入 `ActionGamePacket`，不做解析。
- **去重**：`CachedChunkPacket` 用 64 字节大哈希 + 64 位长哈希两级去重。
- **落盘异步**：`AsyncReplaySaver` 跑独立线程，主线程只 push 数据。

---

## 3. 回放时序（replay flow）

```mermaid
sequenceDiagram
    autonumber
    participant U as 用户
    participant RB as replay-browser
    participant RWM as ReplayWorldManager
    participant IO as ReplayReader
    participant RS as ReplaySession
    participant ACT as ActionRegistry
    participant W as 临时回放世界

    U->>RB: 选择回放
    RB->>RWM: createSpectatorWorld("__playback_replay_world__")
    RWM-->>U: 进入临时世界
    RB->>RS: load(path)
    RS->>IO: open + 读头 / 读 snapshot
    IO-->>RS: 快照区块数据
    RS->>W: injectSnapshot(区块)
    loop 每个 tick
        RS->>IO: readActions()
        IO-->>RS: Action 列表
        loop 每个 Action
            RS->>ACT: getAction(id)
            ACT-->>RS: Action 实例
            RS->>ACT: handle(session, payload)
            ACT->>W: replay packet / move entities / cache chunk
        end
    end
    U->>RB: 退出回放
    RB->>RWM: destroySpectatorWorld()
    RWM-->>U: 返回原世界
```

**要点**

- **世界隔离**：`__playback_replay_world__` 前缀的临时观察者世界，退出即销毁，不污染主存档。
- **回放不解析业务包**：`ActionGamePacket` 在 `ActionRegistry` 查表后用 LeviLamina 提供的 `sendNetworkPacket` 注入，让客户端自己解析。
- **确定性回放**：`ActionNextTick` 控制时间推进，保证 tick 序号与录制严格一致。

---

## 模块索引（速查）

| 模块 | 路径 | 一句话职责 |
|---|---|---|
| `Playback` | `playback/Playback.h` | 单例总入口，串联所有子系统 |
| `PlaybackConfig` | `playback/config.h` | 命令开关 / 重命名静态配置 |
| `command` | `playback/command/` | `playback` 与 `record` 命令注册 |
| `functions/record` | `playback/functions/record/` | 录制状态机 + 网络旁路 + 异步落盘 |
| `functions/replay` | `playback/functions/replay/` | 回放会话 / 驱动 / 临时世界管理 |
| `functions/action` | `playback/functions/action/` | 5 类 Action 协议 + 注册表 |
| `functions/io` | `playback/functions/io/` | 二进制读写 + 缓存 + Snappy |
| `functions/tick` | `playback/functions/tick/` | tick 调度（录制/回放共用） |
| `editor` | `playback/editor/` | ImGui 时间轴 UI（context / controller / renderer / ui） |
| `screen` | `playback/screen/` | 主菜单按钮 + 回放浏览器 |
| `utils` | `playback/utils/` | `PathUtils` / `LinkedHashMap` |
| `resources` | `playback/resources/` | UI 资源包（按钮 / 图标） |

---

## 依赖方向（自上而下，不允许反向）

```
playback (入口)
  ├─ command
  ├─ editor
  ├─ screen
  └─ functions/
       ├─ record ──► action ◄── replay
       │             │           │
       │             ▼           │
       └───────────  io  ◄───────┘
                    │
                    ▼
                  utils
```

- `record` 与 `replay` 是平行模块，**只通过 `action` + `io` 对接**。
- `editor` 只能依赖 `replay`（读状态 / 控播放），不能碰 `record`。
- `utils` 是叶子节点。
