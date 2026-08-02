# 回放时序

## 总览

回放路径分两个阶段：

| 阶段 | 入口 | 输出 |
| --- | --- | --- |
| 启动 | `screen::ReplayBrowser::openReplay` → `ReplaySession::start(path)` | spectator 隔离世界准备就绪 |
| 播放 | `ClientTickHooks::_subTick` → `ReplaySession::tick` | 一帧帧把 Action 应用到世界 |

`ReplaySession` 内部状态比 Recorder 复杂：它同时管"本地世界加载"、"快照复用"、"chunk 注入计划"、"speed/seek/pause"。

## 完整时序图

```mermaid
sequenceDiagram
    autonumber
    participant U as 用户
    participant Menu as MainMenuHooks
    participant Browser as ReplayBrowser
    participant PB as Playback
    participant RS as ReplaySession
    participant SM as MinecraftScreenModel
    participant Lev as LeviLamina 事件总线
    participant Rec as Recorder
    participant RR as ReplayReader<br/>(一个 chunk 一个)
    participant REG as ActionRegistry
    participant Net as NetworkHandler<br/>(LegacyClient)
    participant CT as ClientTickHooks

    rect rgba(120,180,250,0.18)
    note over U,Browser: 1. 主菜单选回放
    U->>Menu: 主菜单点 Playback
    Menu->>Browser: loadReplays() (扫描 data/replays/*.zip)
    Browser-->>Menu: ReplaySummary[] (排序后)
    U->>Menu: 选中 .zip + 双击/点 Open
    Menu->>Browser: openReplay(replay)
    Browser->>PB: 校验 replay.canOpen
    Browser->>RS: ReplaySession::start(path)
    RS->>RS: init(path) <br/>解 zip, 读 metadata.json<br/>对每个 chunk 构造 ReplayReader
    RS->>RS: 读 level_chunk_caches/*.bin<br/>(snappy 解压 -> mChunkPackets)
    RS->>RS: startLocalServerAsync(replayLevelId,<br/>"Playback Replay", spectator + void)
    SM-->>Lev: 异步起本地服务器
    Lev-->>PB: ClientStartJoinLevelEvent
    PB->>RS: onLevelStartJoin() (重置 mScreenModel, 清 network ctx)
    Lev-->>PB: ClientJoinLevelEvent
    PB->>RS: onLevelJoined(player)<br/>记录 mReplayPlayer / mReplayDimension
    PB->>RS: refreshMode(level)<br/>(现在 mode = Replay)
    Lev-->>PB: ClientCommandRegisterEvent
    PB->>Recorder: Recorder::start() 被拒绝<br/>(isReplayMode() = true)
    end

    rect rgba(120,250,160,0.18)
    note over RS,RR: 2. 初始 snapshot 应用
    Lev-->>RS: onWorldReady() (内部触发)
    RS->>RS: applyInitialSnapshot()
    RS->>RR: reader.handleSnapshot(session)
    loop snapshot 内每条 Action
        RR->>REG: action->handle(session, stream)
        REG->>RS: handleLevelChunkCached / SubChunkCached /<br/>handleGamePacket / handleMoveEntities
        RS->>RS: 收集到 mPendingLevelChunkIndices 等队列
    end
    RS->>RS: prepareChunkInjectionPlan(view)<br/>按到 view 距离排序<br/>标记复用列 / 直接应用列 / 注入列
    RS->>Net: injectChunkPacket (LevelChunk) — 限速
    Net-->>RS: onChunkHandleCompleted (NetworkHooks 转发)
    RS->>RS: mCompletedLevelChunkPositions 累计
    RS->>Net: injectReadySubChunkPackets (按依赖)
    RS->>Net: flushPendingSnapshotGamePackets<br/>(PlayerList 先 -> 实体后)
    RS->>RS: finishChunkInjection() 清理<br/>清空 recorded entities
    end

    rect rgba(250,220,120,0.18)
    note over CT,RS: 3. 每个 client tick 推进
    loop ClientTickHooks::_subTick
        CT->>RS: ReplaySession::tick()
        alt seek 目标存在
            RS->>RS: while mCurrentTick < mSeekTargetTick
            RS->>RS: advanceReplayTick(false) — 连续快进
        else 普通播放
            RS->>RS: mPlaybackTickAccumulator += mPlaybackSpeed
            loop ticksToAdvance
                RS->>RS: advanceReplayTick(true)
                RS->>RR: reader.handleNextAction(session)
                RR->>REG: action->handle(session, stream)
                REG->>RS: handleNextTick (mCurrentTick++)<br/>或 handleLevelChunkCached/.../handleMoveEntities
                RS->>Net: applyGamePacket (via packet->mHandler)
                end
            end
        end
    end

    CT->>RS: 注入 chunk (如 handleLevelChunkCached 触发)
    RS->>RS: mChunkInjectionPending = true
    RS->>RS: 后续 tick 完成 prepareChunkInjectionPlan + 注入
    end

    rect rgba(250,120,160,0.18)
    note over U,RS: 4. 退出
    U->>RS: EditorAction::StopReplay
    RS->>RS: stop()
    RS->>RS: requestLeaveGameAsync()
    Lev-->>PB: ClientExitLevelEvent
    PB->>RS: onLevelExit() -> ReadyToDelete
    Lev-->>PB: ClientStartJoinLevelEvent<br/>(重新进主菜单)
    PB->>RS: onLevelStartJoin() -> finishWorldCleanup
    PB->>RS: tryFinalizeWorldCleanup (从 worldsPath 删 __playback_replay_world__*)
    end
```

## 关键细节

### ReplaySession 内部状态

最关键的几个开关（`mChunkInjectionPending` / `mApplyingChunkSnapshot` / `mSnapshotGamePacketPhase` / `mCenterChunksReady`）控制 chunk 注入流程：

| 状态 | 含义 |
| --- | --- |
| `mInitialSnapshotApplied` | 是否已应用过首块 snapshot |
| `mChunkInjectionPending` | 还有 chunk 等待注入 |
| `mApplyingChunkSnapshot` | 当前正在应用 snapshot（非增量） |
| `mSnapshotGamePacketPhase` | `StreamingChunks` / `WaitingAfterPlayerList` / `WaitingAfterEntities` |
| `mCenterChunksReady` | 中心 5×5 chunk 已就绪 |
| `mSeekTargetTick` | ≥0 时进入快进模式 |
| `mIsPaused` / `mPlaybackSpeed` | 用户控制 |

### Chunk 注入计划

`prepareChunkInjectionPlan` 把 snapshot/timeline 的 chunk 分类：

- **复用列 (`mReusableSnapshotColumns`)**：和上一次 snapshot 完全一致且没被破坏（`mDirtySnapshotColumns` 为空），跳过注入。
- **直接应用列 (`mDirectSnapshotColumns` / `mDirectLevelChunkIndices`)**：维度匹配 + 高度全覆盖 + chunk 已 Loaded，直接 `chunk->deserializeBiomes/deserializeBorderBlocks` 或 `_handleSubChunkData`。
- **注入列**：通过 `LegacyClientNetworkHandler` 把完整 `LevelChunkPacket`/`SubChunkPacket` 喂给客户端。

中心 5×5 优先（预算 8ms / tick），外圈继续（预算 4ms / tick）。

### 网络包隔离

`NetworkHooks` 中的 `PlaybackLevelChunkHook` / `PlaybackSubChunkHook` / `PlaybackSetTimeHook` 在 `shouldIsolateChunkPackets()` 为 true 时抑制原生包：

- `LevelChunk`：未注入过的列才放行（避免原生抢先把未录制区块填进回放世界）。
- `SubChunk`：过滤掉已被 snapshot 占用的条目。
- `SetTime`：在隔离回放世界时直接 drop。

`mInjectingPacket` 原子变量标记"当前是 ReplaySession 自己注入的包"，让 hook 自识别不重入。

### 时间轴控制

- `adjustPlaybackSpeed(direction)`：从 `PlaybackSpeeds = {0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0}` 中按"距离最近"原则取一个。
- `requestSeek(tick)`：写 `mRequestedSeekTick`，`beginSeek` 找到目标 tick 所在的 chunk（reader），如果回退则重新 apply snapshot，否则只设 `mSeekTargetTick` 让 `tick` 主循环连续快进。
- `EditorController` 把 UI 的 `EditorAction` 翻译成 `setPaused` / `requestSeek` / `adjustPlaybackSpeed` / `requestStop` / `requestSeek(0/getTotalTicks())`。

### 隔离世界清理

`__playback_replay_world__<uuid>` 命名规则保证回放世界在 `worldsPath` 下可识别。`tryFinalizeWorldCleanup` 在每次 `ClientUpdate` 末尾跑：

- `None` → 扫描 `worldsPath` 找遗留孤立回放世界，标记 `ReadyToDelete`。
- `WaitingForExit` → 用户已点退出，转 `ReadyToDelete`。
- `ReadyToDelete` → `cache.deleteLevel(mReplayLevelId)`；等 `REPLAY_WORLD_DELETE_TIMEOUT_TICKS = 20*30` 超时则报错。

## 回放相关模块

- 协议层：[functions/action.md](../functions/action.md)
- 录制对应的回放数据来源：[functions/io.md](../functions/io.md)（`ReplayReader`）
- ReplaySession 状态机：[functions/replay.md](../functions/replay.md)
- Tick 触发与回放/录制分发：[functions/tick.md](../functions/tick.md)
- 编辑器状态机：[editor/context.md](../editor/context.md)、[editor/controller.md](../editor/controller.md)
- 主菜单入口：[screen/replay-browser.md](../screen/replay-browser.md)
- NetworkHooks（隔离世界逻辑）：[functions/record.md](../functions/record.md)
