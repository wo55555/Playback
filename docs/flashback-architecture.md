# Flashback 回放架构文档

## 一、项目概述

Flashback 是一个 Minecraft Fabric 模组，提供游戏录像（录制）、回放与编辑功能。其核心原理是：**在录制阶段截获服务端发往客户端的所有网络包**，连同实体位置、玩家状态等游戏状态数据序列化到 `.flashback` 二进制文件中；在回放阶段，创建一个本地集成服务器 (`ReplayServer`)，将录制时的网络包按原始顺序重放，从而精确复现游戏画面。

项目基于 **Java + Fabric Loader + Mixin（字节码注入）**，重度依赖 Minecraft 原版的网络包系统与客户端-服务端架构。

---

## 二、项目顶层结构

```
com.moulberry.flashback/
├── Flashback.java              # 主入口，实现 ModInitializer & ClientModInitializer
├── action/                     # 动作系统（录制数据的原子单元）
├── io/                         # 二进制读写（ReplayWriter / ReplayReader / AsyncReplaySaver）
├── record/                     # 录制引擎（Recorder, FlashbackMeta, ReplayExporter）
├── playback/                   # 回放引擎（ReplayServer, ReplayPlayer, ReplayGamePacketHandler）
├── state/                      # 编辑器状态管理（EditorState, EditorScene, KeyframeTrack）
├── editor/                     # 编辑器 UI（ReplayUI, SavedTrack, CopiedKeyframes）
├── keyframe/                   # 关键帧系统（Camera, FOV, Speed, TimeOfDay 等）
├── mixin/                      # Mixin 字节码注入（record/ playback/ visuals/ 等）
├── packet/                     # 自定义网络包（FlashbackForceClientTick 等）
├── exporting/                  # 导出/渲染（ExportJob, PerfectFrames）
├── configuration/              # 配置系统
├── compat/                     # 兼容层（DistantHorizons, Bobby, SimpleVoiceChat）
├── serialization/              # 序列化工具
├── visuals/                    # 视觉效果（ReplayVisuals, AccurateEntityPositionHandler）
└── screen/                     # 自定义屏幕（SaveReplayScreen, RecoverRecordingsScreen 等）
```

---

## 三、录制系统 (Recording System)

### 3.1 架构概述

录制的核心是 **截获客户端收到的每一个网络包**。Minecraft 的网络通信模型是：服务端发送 `Packet` 对象到客户端，经过 Netty 管道序列化并传输。Flashback 通过 `MixinConnection` 注入到 Minecraft 的网络层 `Connection.genericsFtw()` 方法中，在数据包到达客户端 PacketListener **的同时**将其加入录制队列。

```
服务端 → Netty → Connection.genericsFtw(packet) → ClientPacketListener.handle(packet)
                              ↓ Mixin 注入
                         Recorder.writePacketAsync(packet)
```

### 3.2 核心类

#### 3.2.1 `Flashback.java` — 录制生命周期管理

关键字段：

```java
public static volatile Recorder RECORDER = null;  // 全局录制器实例（null 表示未录制）
private static int delayedStartRecording = 0;      // 延迟启动计时器
```

录制控制方法：

| 方法                            | 说明                                                                                                   |
| ------------------------------- | ------------------------------------------------------------------------------------------------------ |
| `startRecordingReplay()`        | 创建 `Recorder` 实例，开始录制                                                                         |
| `finishRecordingReplay()`       | 调用 `recorder.endTickWithContext(true)`，写入最后数据，然后通过 `ReplayExporter.export()` 导出为 .zip |
| `pauseRecordingReplay(boolean)` | 设置暂停标志，暂停期间不写入 tick 数据                                                                 |
| `cancelRecordingReplay()`       | 丢弃录制数据，删除临时文件夹                                                                           |

自动录制逻辑（在 `ClientTickEvents.START_CLIENT_TICK` 中）：
1. 检查 `automaticallyStart` 配置项
2. 若开启，加入世界 20 tick 后自动调用 `startRecordingReplay()`

#### 3.2.2 `Recorder.java` — 录制引擎

核心常量与字段：

```java
public static final int CHUNK_LENGTH_SECONDS = 5 * 60;  // 每个 chunk 5 分钟
private final AsyncReplaySaver asyncReplaySaver;          // 异步写入器
private final Queue<PacketWithPhase> pendingPackets;      // 待处理包队列
private int writtenTicks = 0;                             // 已写入总 tick 数
private int writtenTicksInChunk = 0;                      // 当前 chunk 内已写入 tick 数
private boolean needsInitialSnapshot = true;              // 是否需要初始快照
private boolean isPaused = false;                         // 暂停标志
private boolean isConfiguring = false;                    // 是否处于配置阶段
```

**录制流程（每个客户端 tick 调用 `endTick()`）：**

```
1. writeSnapshot(true)          ← 第一个 tick 写入初始快照
2. flushPackets()               ← 刷新待处理的网络包队列
3. writeEntityPositions()       ← 写入发生变化的实体位置
4. writeLocalData()             ← 写入本地玩家数据
5. writeAccurateFirstPersonPosition() ← 可选的精准第一人称位置
6. asyncReplaySaver.submit(writer -> writer.startAndFinishAction(ActionNextTick.INSTANCE))
7. 若距上次 chunk >= 5分钟或暂停结束 → writeReplayChunk() + 写入新快照
```

**快照机制 (`writeSnapshot`)：**

快照是**完整游戏状态的序列化**，用于回放时的"跳转"操作。当用户拖动时间轴到任意位置，回放系统从最近的快照开始恢复状态。快照包含：

- 配置阶段数据（启用的 FeatureFlags、注册表数据、Tags、资源包）
- Login 包（玩家 ID、维度信息、游戏模式等）
- 本地玩家创建数据（UUID、位置、GameProfile、游戏模式）
- 玩家列表信息（PlayerInfoUpdatePacket）
- Tab 列表自定义
- BossBar 状态
- 记分板（Objective、Team）
- 地图数据（MapItemSavedData）
- 世界信息（WorldBorder、GameRules、Difficulty、TimeOfDay 等）
- 所有已加载区块（LevelChunk）及光照数据
- 所有实体（Entity）的创建包 + 实体数据 + 装备 + 属性 + 效果 + 乘客关系
- 自定义 Payload 包

**实体位置录制 (`writeEntityPositions`)：**

```java
for (Entity entity : level.entitiesForRendering()) {
    Position position = new Position(x, y, z, yaw, pitch, headYRot, onGround);
    Position lastPosition = this.lastPositions.get(entity);
    if (!Objects.equals(position, lastPosition)) {
        // 仅写入位置发生变化的实体
        changedPositions.add(new IdWithPosition(entity.getId(), position));
    }
}
// 写入 ActionMoveEntities，包含维度信息 + 实体 ID + 完整位置
```

位置数据按维度分组写入，每个维度包含多个实体的 (id, x, y, z, yaw, pitch, headYaw, onGround)。

**本地玩家数据录制 (`writeLocalData`)：**

录制以下变化的客户端状态作为网络包：

| 数据类型       | 对应数据包                          | 变化检测字段                                             |
| -------------- | ----------------------------------- | -------------------------------------------------------- |
| 经验值         | `ClientboundSetExperiencePacket`    | `lastExperienceProgress/TotalExperience/ExperienceLevel` |
| 生命值/饥饿值  | `ClientboundSetHealthPacket`        | `lastFoodLevel/lastSaturationLevel`                      |
| 选中物品栏槽位 | `ClientboundSetHeldSlotPacket`      | `lastSelectedSlot`                                       |
| 实体元数据     | `ClientboundSetEntityDataPacket`    | `lastPlayerEntityMeta`                                   |
| 装备           | `ClientboundSetEquipmentPacket`     | `lastPlayerEquipment`                                    |
| 物品栏         | `ClientboundContainerSetSlotPacket` | `lastHotbarItems`                                        |
| 方块破坏进度   | `ClientboundBlockDestructionPacket` | `lastDestroyPos/lastDestroyProgress`                     |
| 挥动手臂       | `ClientboundAnimatePacket`          | `wasSwinging/lastSwingTime`                              |
| 运动向量       | `ClientboundSetEntityMotionPacket`  | 每 tick 写入                                             |

**精准第一人称位置 (`writeAccurateFirstPersonPosition`)：**

当 `localPlayerUpdatesPerSecond > 20` 时启用。使用帧间插值记录更流畅的玩家视角移动：

1. 收集 tick 内的偏量位置 `partialPositions`（通过 `trackPartialPosition` 在渲染帧中收集）
2. 根据配置的更新频率计算插值分段数
3. 对每个分段在 `lastPosition` 和 `nextPosition` 之间做线性插值
4. 将所有插值位置打包为 `FlashbackAccurateEntityPosition` 包
5. 通过 `ActionAccuratePlayerPosition` 写入

#### 3.2.3 `MixinConnection` — 网络包截获

```java
@Inject(method = "genericsFtw", at = @At("HEAD"))
private static void genericsFtw(Packet<?> packet, PacketListener packetListener, CallbackInfo ci) {
    Recorder recorder = Flashback.RECORDER;
    if (recorder != null) {
        if (packetListener instanceof ClientGamePacketListener) {
            recorder.writePacketAsync(packet, ConnectionProtocol.PLAY);
        } else if (packetListener instanceof ClientConfigurationPacketListener) {
            recorder.writePacketAsync(packet, ConnectionProtocol.CONFIGURATION);
        }
    }
}
```

`writePacketAsync` 的过滤逻辑：

- 展开 `ClientboundBundlePacket` 为子包
- 将 `ClientboundPlayerChatPacket` 转为 `ClientboundSystemChatPacket`
- 忽略 `fabric-screen-handler-api` 命名空间的 CustomPayload
- 忽略本地玩家自身的 `SetEntityData` 和 `SetEquipment`（由 `writeLocalData` 单独处理）
- 忽略 `IgnoredPacketSet` 中标记的无关包

#### 3.2.4 `MixinClientLevel` — 客户端侧事件录制

- `setBlock`：截获方块预测（prediction）产生的方块变更，写入 `ClientboundBlockUpdatePacket`
- `levelEvent` / `globalLevelEvent`：录制世界事件（声音、粒子效果等）
- `playSeededSound`：录制种子化声音（包含位置/实体 + 种子值，确保回放一致）

### 3.3 录制文件格式

录制过程中数据先写入临时文件夹，结构如下：

```
flashback/recordings/<uuid>/
├── metadata.json           # FlashbackMeta 元数据
├── metadata.json.old       # 备份
├── c0.flashback            # 第一个 chunk（5分钟）
├── c1.flashback            # 第二个 chunk
├── ...
├── level_chunk_caches/
│   ├── 0.bin               # 区块缓存（按需分片）
│   └── ...
└── icon.png                # 缩略图截图
```

录制结束后 `ReplayExporter.export()` 将所有文件打包为单个 `.zip`：

```
replay_name.zip
├── metadata.json
├── c0.flashback
├── c1.flashback
├── ...
├── 0.bin
├── 1.bin
└── icon.png
```

---

## 四、IO 系统 (Binary Format)

### 4.1 `ReplayWriter.java` — 二进制写入器

**文件格式（.flashback）：**

```
┌────────────────────────────────────┐
│  Magic Number (int32): 0xD780E884  │
├────────────────────────────────────┤
│  Action Count (varint)             │
│  Action Registry:                  │
│    Action 0: Identifier            │
│    Action 1: Identifier            │
│    ...                             │
├────────────────────────────────────┤
│  Snapshot Size (int32)             │
│  Snapshot Data:                    │
│    [Action 0]: varint id + int32 size + byte[] data
│    [Action 1]: varint id + int32 size + byte[] data
│    ...                             │
├────────────────────────────────────┤
│  Action Data (逐 tick):            │
│    [Action]: varint id + int32 size + byte[] data
│    ...                             │
└────────────────────────────────────┘
```

关键方法：

| 方法                                           | 说明                                            |
| ---------------------------------------------- | ----------------------------------------------- |
| `startSnapshot()` / `endSnapshot()`            | 标记快照区域边界，写入占位大小后回填            |
| `startAction(Action)` / `finishAction(Action)` | 写入 Action ID + 占位大小 → 填充数据 → 回填大小 |
| `startAndFinishAction(Action)`                 | 写入没有数据的 Action（如 `ActionNextTick`）    |
| `friendlyByteBuf()`                            | 获取当前写入缓冲区                              |

状态机：

```
STATE_EMPTY → (startSnapshot) → STATE_WRITING_SNAPSHOT
STATE_WRITING_SNAPSHOT → (endSnapshot) → STATE_WRITING_DATA
```

### 4.2 `ReplayReader.java` — 二进制读取器

构造时读取并解析 Header：
1. 验证 Magic Number
2. 解析 Action Registry，建立 `int id → Action` 映射
3. 读取快照大小，计算 `replaySnapshotOffset` 和 `replayActionsOffset`

关键方法：

| 方法                             | 说明                                                          |
| -------------------------------- | ------------------------------------------------------------- |
| `handleSnapshot(ReplayServer)`   | 从快照偏移开始，逐 Action 调用 `action.handle()` 恢复状态     |
| `handleNextAction(ReplayServer)` | 从当前位置读取下一个 Action 并执行，返回 `false` 表示到达末尾 |
| `resetToStart()`                 | 重置读取位置到 Action 数据区开头                              |

### 4.3 `AsyncReplaySaver.java` — 异步保存器

生产者-消费者模式：

- **生产者**（客户端主线程）：调用 `submit(Consumer<ReplayWriter>)` 将写入任务放入 `ArrayBlockingQueue`
- **消费者**（后台线程 `AsyncReplayWriter`）：循环从队列取出任务并执行

区块缓存优化：对于 `ClientboundLevelChunkWithLightPacket`，使用 `LongOpenHashSet` 做去重，相同的区块数据只存储一次，通过索引引用（`ActionLevelChunkCached`）。

---

## 五、动作系统 (Action System)

### 5.1 `Action.java` 接口

```java
public interface Action {
    Identifier name();
    void handle(ReplayServer replayServer, RegistryFriendlyByteBuf friendlyByteBuf);
}
```

### 5.2 `ActionRegistry.java`

在 `Flashback.onInitializeClient()` 中注册所有 Action：

```
ActionNextTick            → replayServer.handleNextTick()
ActionGamePacket          → replayServer.handleGamePacket(friendlyByteBuf)
ActionConfigurationPacket → replayServer.handleConfigurationPacket(friendlyByteBuf)
ActionCreateLocalPlayer   → replayServer.handleCreateLocalPlayer(friendlyByteBuf)
ActionMoveEntities        → replayServer.handleMoveEntities(friendlyByteBuf)
ActionLevelChunkCached    → replayServer.handleLevelChunkCached(index)
ActionAccuratePlayerPosition → replayServer.handleAccuratePlayerPosition(friendlyByteBuf)
```

### 5.3 各 Action 详解

#### `ActionNextTick`
无数据体。调用 `replayServer.handleNextTick()` 推进回放 tick。

#### `ActionGamePacket`
数据体是序列化的游戏阶段数据包。读取时调用 `gamePacketCodec.decode(friendlyByteBuf)` 反序列化并传给 `ReplayGamePacketHandler`。

#### `ActionConfigurationPacket`
数据体是序列化的配置阶段数据包。读取时调用 `configurationPacketCodec.decode(friendlyByteBuf)` 反序列化并传给 `ReplayConfigurationPacketHandler`。

#### `ActionCreateLocalPlayer`
数据体包含：
- UUID (16 bytes)
- Double x, y, z (24 bytes)
- Float yaw, pitch, headYaw (12 bytes)
- Double motionX, motionY, motionZ (24 bytes)
- GameProfile (via `ByteBufCodecs.GAME_PROFILE`)
- VarInt gameModeId

#### `ActionMoveEntities`
按维度分组写入实体位置变化：
```
VarInt levelCount
For each level:
  ResourceKey<Level> dimension
  VarInt entityCount
  For each entity:
    VarInt entityId
    Double x, y, z
    Float yaw, pitch, headYaw
    Boolean onGround
```

#### `ActionLevelChunkCached`
数据体为一个 VarInt 索引，指向区块缓存文件中的 chunk 数据。回放时若该 tick 的区块数据与缓存匹配则跳过重复发送。

#### `ActionAccuratePlayerPosition`
数据体为 `FlashbackAccurateEntityPosition` 序列化（entityId + interpolated positions list）。

---

## 六、回放系统 (Playback System)

### 6.1 架构概述

回放的核心是 `ReplayServer`，它继承自 Minecraft 原版的 `IntegratedServer`（本地集成服务器）。当用户打开回放文件时：

1. 用 `FileSystems.newFileSystem(path)` 以 ZIP 文件系统方式打开 .zip
2. 读取 `metadata.json` 获取所有 chunk 信息
3. 创建 `ReplayServer` 实例
4. Minecraft 客户端连接到这个本地服务器
5. 服务器逐 tick 读取 `.flashback` 文件中的 Action 并执行

```
ReplayServer (extends IntegratedServer)
    │
    ├── tickServer()          ← 每个服务器 tick
    │   ├── 处理快照初始化
    │   ├── 关键帧应用
    │   ├── handleActions()   ← 读取/解析 Action
    │   ├── 实体位置广播
    │   └── 第一人称数据同步
    │
    ├── ReplayGamePacketHandler      ← 游戏包处理器
    ├── ReplayConfigurationPacketHandler ← 配置包处理器
    ├── ReplayPlayer (x N)           ← 回放观看者
    └── ReplayChunkCache             ← 区块缓存
```

### 6.2 `ReplayServer.java` 核心方法

#### 6.2.1 构造与初始化

```java
public ReplayServer(Thread thread, Minecraft minecraft, ..., UUID playbackUUID, Path path) {
    // 1. 创建包处理器
    this.gamePacketHandler = new ReplayGamePacketHandler(this);
    this.configurationPacketHandler = new ReplayConfigurationPacketHandler(this);

    // 2. 以 ZIP 文件系统打开回放文件
    this.playbackFileSystem = FileSystems.newFileSystem(path);

    // 3. 读取 metadata.json
    String metadataJson = Files.readString(metadataPath);
    this.metadata = FlashbackMeta.fromJson(GSON.fromJson(metadataJson, JsonObject.class));

    // 4. 构建 PlayableChunk 映射 (起始 tick → chunk 信息)
    int ticks = 0;
    for (Map.Entry<String, FlashbackChunkMeta> entry : this.metadata.chunks.entrySet()) {
        this.playableChunksByStart.put(ticks, new PlayableChunk(...));
        ticks += entry.getValue().duration;
    }
    this.totalTicks = ticks;
}
```

#### 6.2.2 `tickServer()` — 主循环

```java
public void tickServer(BooleanSupplier booleanSupplier) {
    // === 阶段 0: 首次初始化 ===
    if (!this.initializedWithSnapshot) {
        this.initializedWithSnapshot = true;
        ReplayReader replayReader = this.playableChunksByStart.get(0).getOrLoadReplayReader(...);
        replayReader.handleSnapshot(this);  // 播放初始快照
        this.gamePacketHandler.flushPendingEntities();
    }

    // === 阶段 1: 更新回放观看者列表 ===
    this.replayViewers.clear();
    for (ServerPlayer player : this.getPlayerList().getPlayers()) {
        if (player instanceof ReplayPlayer replayPlayer) {
            // 跟踪观看者 spectator 状态
            this.replayViewers.add(replayPlayer);
        }
    }

    // === 阶段 2: 更新当前 tick ===
    if (this.jumpToTick >= 0) {
        this.targetTick = this.jumpToTick;  // 跳转
        this.jumpToTick = -1;
    } else if (!this.replayPaused && this.targetTick < this.totalTicks) {
        this.targetTick += 1;  // 正常播放
    }

    // === 阶段 3: 运行更新 ===
    if (正常播放/跳转/导出/冻结) {
        this.runUpdates(booleanSupplier);  // 单次 tick 更新
    } else {
        // 追赶模式：以每次 +20 ticks 加速追赶
        while (this.targetTick <= realTargetTick) {
            this.fastForwarding = true;
            this.runUpdates(booleanSupplier);
            this.targetTick += 1;
        }
    }

    // === 阶段 4: 第一人称数据同步 ===
    for (ReplayPlayer replayViewer : this.replayViewers) {
        // 同步经验值、饥饿值、物品栏选择、物品栏内容
    }

    // === 阶段 5: 资源包同步 ===
    tickResourcePacks(editorState);

    // === 阶段 6: 发送 FinishedServerTick ===
    if (this.sendFinishedServerTick.compareAndExchange(true, false)) {
        for (ReplayPlayer replayViewer : this.replayViewers) {
            ServerPlayNetworking.send(replayViewer, FinishedServerTick.INSTANCE);
        }
    }
}
```

#### 6.2.3 `runUpdates()` — 单次更新逻辑

```java
private void runUpdates(BooleanSupplier booleanSupplier) {
    // 1. 应用关键帧
    this.getEditorState().applyKeyframes(new ReplayServerKeyframeHandler(this), this.targetTick);

    // 2. 处理冻结延迟
    if (this.desiredFrozen && this.frozenDelay < 0) {
        this.frozenDelay = 1~3; // 根据 desiredFrozenDelay 设置
    }

    // 3. 更新 tick rate
    float tickRate = this.desiredTickRate;
    if (Flashback.EXPORT_JOB == null) {
        tickRate *= this.desiredTickRateManual / 20f;
    }
    tickRateManager.setTickRate(tickRate);
    tickRateManager.setFrozen(isFrozen);

    // 4. 处理 Action（读取回放数据）
    if (!isFrozen) {
        this.handleActions();
    }

    // 5. 保持实体加载
    for (ServerLevel level : this.getAllLevels()) {
        for (Entity entity : level.getAllEntities()) {
            level.getChunkSource().addTicketWithRadius(ENTITY_LOAD_TICKET, chunkPos, 3);
        }
    }

    // 6. Tick 底层服务器
    super.tickServer(booleanSupplier);

    // 7. 应用 BlockOverride 关键帧
    applyBlockOverridesToTimeline();

    // 8. 广播实体位置更新
    if (!this.needsPositionUpdate.isEmpty()) {
        for (Map.Entry<ResourceKey<Level>, IntSet> entry : this.needsPositionUpdate.entrySet()) {
            // 发送 ClientboundEntityPositionSyncPacket / ClientboundMoveEntityPacket.Rot
        }
        this.needsPositionUpdate.clear();
    }

    // 9. 快照后清理
    if (this.processedSnapshot) {
        // 强制客户端 tick、清除粒子、实体插值回正
    }

    // 10. 暂停时冻结 tick rate
    if (this.replayPaused && !tickRateManager.isFrozen()) {
        tickRateManager.setFrozen(true);
    }
}
```

#### 6.2.4 `handleActions()` — 动作处理引擎

```java
private void handleActions() {
    // 1. 判断是否需要跳转到新 chunk
    boolean shouldJump = this.targetTick < this.currentTick
        || this.targetTick > this.currentTick + duration
        || oldChunk != targetChunk;

    if (shouldJump) {
        // 通过播放快照重置状态
        Map.Entry<Integer, PlayableChunk> entry = this.playableChunksByStart.floorEntry(this.targetTick);
        this.playSnapshot(entry.getValue().getOrLoadReplayReader(...));
        this.currentTick = entry.getKey();
    }

    // 2. 逐 Action 读取
    while (this.currentTick < this.targetTick) {
        // 应用 BlockOverride 关键帧（如果 tick 有变化）
        applyBlockOverrideKeyframes(blockOverrideKeyframes, lastBlockOverrideTick);

        if (!this.currentReplayReader.handleNextAction(this)) {
            // 当前 chunk 读完，切换到下一个
            Map.Entry<Integer, PlayableChunk> newEntry = this.playableChunksByStart.floorEntry(this.currentTick);
            this.currentReplayReader = newEntry.getValue().getOrLoadReplayReader(...);
            this.currentReplayReader.resetToStart();
        }
    }

    // 3. 最后一次 BlockOverride 应用
    applyBlockOverrideKeyframes(blockOverrideKeyframes, this.currentTick);
}
```

#### 6.2.5 `handleMoveEntities()` — 实体位置处理

```java
public void handleMoveEntities(RegistryFriendlyByteBuf friendlyByteBuf) {
    int levelCount = friendlyByteBuf.readVarInt();
    for (int i = 0; i < levelCount; i++) {
        ResourceKey<Level> dimension = friendlyByteBuf.readResourceKey(Registries.DIMENSION);
        ServerLevel level = this.levels.get(dimension);

        int count = friendlyByteBuf.readVarInt();
        for (int j = 0; j < count; j++) {
            int id = friendlyByteBuf.readVarInt();
            double x,y,z; float yaw,pitch,headYaw; boolean onGround;

            Entity entity = level.getEntity(id);
            if (entity != null) {
                entity.snapTo(x, y, z, yaw, pitch);  // 设置位置
                entity.setYHeadRot(headYaw);
                // 标记需要位置同步
                positionUpdateSet.add(id);
            } else if (!this.isFrozen) {
                // 未知实体，广播 TeleportPacket 创建
                this.getPlayerList().broadcastAll(
                    PacketHelper.createTeleportForUnknown(id, x, y, z, yRot, xRot, onGround)
                );
            }
        }
    }
}
```

### 6.3 `ReplayGamePacketHandler.java` — 游戏包处理器

实现 `ClientGamePacketListener`，在回放服务器侧处理所有游戏包。

**核心设计原则**：将回放服务器的数据包"转发"给所有 ReplayPlayer（回放观看者）。

```java
private void forward(Packet<?> packet) {
    for (ServerPlayer replayViewer : this.replayServer.getReplayViewers()) {
        replayViewer.connection.send(packet);
    }
}
```

**待处理实体系统 (`pendingEntities`)**：

因为原版数据包之间可能存在依赖关系（例如实体数据包在实体创建包之前到达），所以采用延迟刷新策略：

1. 实体创建包到达时，先创建实体放入 `pendingEntities` 映射
2. 遇到下一个不允许延迟的包时，调用 `flushPendingEntities()` 真正添加所有实体
3. 若已存在同类型实体，使用 `restoreFrom()` 恢复状态而非重新创建

**特殊处理：**

- `ClientboundLevelChunkWithLightPacket`：通过 `setBlockStateWithoutUpdates` 直接设置方块状态
- `ClientboundBlockUpdatePacket`：同样直接更新方块状态
- `ClientboundCustomPayloadPacket`：做特殊编码处理避免重序列化
- `ClientboundBossEventPacket`：转发给 `ReplayServer.updateBossBar()` 维护 BossBar 状态
- `ClientboundSetEntityDataPacket`：本地玩家数据由 `writeLocalData` 处理，此处跳过
- `ClientboundSetPlayerTeamPacket`：维护记分板 Team 状态
- `ClientboundSetObjectivePacket`：维护记分板 Objective 状态

### 6.4 `ReplayPlayer.java` — 回放观看者

扩展 `ServerPlayer`，关键特性：

- **无敌**：`isInvulnerableTo()` 始终返回 `true`
- **无伤害**：`hurtServer()` 和 `hurtClient()` 返回 `false`
- **无统计/成就**：`awardStat()` / `awardRecipes()` 为空操作
- **Spectator 跟踪**：`spectatingUuid` / `forceRespectateTickCount` 实现持续 spectator 跟踪
- **第一人称数据缓存**：`lastFirstPersonHotbarItems` / `lastFirstPersonExperienceLevel` 等

---

## 七、编辑器系统 (Editor System)

### 7.1 `EditorState.java` — 编辑器状态

每个回放文件对应一个 `EditorState`，存储在 `flashback/editor_states/<uuid>.json`。

**字段分类：**

| 类别     | 字段                                                                                       | 说明                                   |
| -------- | ------------------------------------------------------------------------------------------ | -------------------------------------- |
| 场景     | `scenes` (List\<EditorScene\>)                                                             | 多场景支持                             |
| 视觉效果 | `replayVisuals`                                                                            | 覆盖 FOV、天气模式、禁用服务器资源包等 |
| 实体过滤 | `hideDuringExport`, `filteredEntities`, `filteredParticles`                                | 导出时隐藏/过滤                        |
| 外观覆盖 | `skinOverride`, `nameOverride`, `glowingOverride`                                          | 皮肤/名称/发光覆盖                     |
| 隐藏选项 | `hideNametags`, `hideCape`, `hideTeamPrefix/Suffix`, `hiddenEquipment`, `hiddenModelParts` | 各种隐藏                               |
| 音频     | `audioSourceEntity`, `muteVoice`                                                           | 音频源/静音                            |
| 缩放     | `zoomMin`, `zoomMax`                                                                       | 时间轴缩放                             |
| 元数据   | `usedByPaths`                                                                              | 跟踪关联的回放文件路径                 |

### 7.2 `EditorStateManager.java` — 状态管理器

线程安全的状态管理器，使用 `ReentrantLock`。

**关键方法：**

```java
// 获取当前回放的编辑器状态
public static EditorState get(UUID uuid) { ... }

// 自动保存（30秒间隔）
public static void saveIfNeeded() { ... }

// 重置（播放其他回放时）
public static void reset() { ... }
```

**持久化策略**：
1. 主文件：`<uuid>.json`
2. 备份文件：`<uuid>.json.old`（保存前先 rename 原文件）

### 7.3 `EditorScene.java` — 编辑器场景

```java
public class EditorScene {
    public String name;
    public final List<KeyframeTrack> keyframeTracks = new ArrayList<>();
    public int exportStartTicks = -1;  // 导出起始 tick
    public int exportEndTicks = -1;    // 导出结束 tick
    private final EditorSceneHistory history;  // 撤销/重做历史
}
```

**撤销/重做系统**：通过 `EditorSceneHistory` 记录 `EditorSceneHistoryEntry`（包含 undo/redo Action 列表）。

---

## 八、关键帧系统 (Keyframe System)

### 8.1 类型注册

在 `Flashback.onInitializeClient()` 中注册的关键帧类型：

| 类型             | 类                          | 功能                                       |
| ---------------- | --------------------------- | ------------------------------------------ |
| `camera`         | `CameraKeyframeType`        | 相机位置/旋转（支持 x/y/z/yaw/pitch/roll） |
| `camera_orbit`   | `CameraOrbitKeyframeType`   | 轨道相机（围绕目标点旋转）                 |
| `track_entity`   | `TrackEntityKeyframeType`   | 追踪实体                                   |
| `camera_shake`   | `CameraShakeKeyframeType`   | 相机震动                                   |
| `fov`            | `FOVKeyframeType`           | 视野角度                                   |
| `speed`          | `SpeedKeyframeType`         | 回放速度                                   |
| `timelapse`      | `TimelapseKeyframeType`     | 延时摄影                                   |
| `time_of_day`    | `TimeOfDayKeyframeType`     | 游戏内时间                                 |
| `freeze`         | `FreezeKeyframeType`        | 冻结回放                                   |
| `block_override` | `BlockOverrideKeyframeType` | 方块状态覆盖（内置）                       |
| `audio`          | `AudioKeyframeType`         | 音频播放                                   |

### 8.2 `Keyframe.java` — 关键帧基类

```java
public abstract class Keyframe {
    private InterpolationType interpolationType = InterpolationType.getDefault();

    public abstract KeyframeType<?> keyframeType();
    public abstract Keyframe copy();
    public abstract KeyframeChange createChange();
    public abstract KeyframeChange createSmoothInterpolatedChange(...);
    public abstract KeyframeChange createHermiteInterpolatedChange(...);
}
```

**插值类型 (`InterpolationType`)**：

| 类型          | 时间轴图标          | 说明                 |
| ------------- | ------------------- | -------------------- |
| `LINEAR`      | ◁▷ 菱形             | 线性插值             |
| `SMOOTH`      | ● 圆                | Catmull-Rom 平滑插值 |
| `EASE_IN`     | ◁▷ 左侧三角右侧箭头 | 缓入                 |
| `EASE_OUT`    | ▷◁ 左侧箭头右侧三角 | 缓出                 |
| `EASE_IN_OUT` | ◁◁ 两侧三角         | 缓入缓出             |
| `HOLD`        | ■ 方形              | 保持（突变）         |
| `HERMITE`     | ▲ 三角              | Hermite 曲线插值     |

### 8.3 `KeyframeChange` 与应用流程

每个关键帧类型通过 `createChange()` 等方法生成 `KeyframeChange` 对象。`EditorState.applyKeyframes(KeyframeHandler, tick)` 遍历所有关键帧轨道，计算当前 tick 对应的变化并应用到 `KeyframeHandler`。

**两种 Handler：**
- `ReplayServerKeyframeHandler`：在回放服务器 tick 中应用（速度、冻结、时间、BlockOverride）
- `MinecraftKeyframeHandler`：在客户端渲染帧中应用（相机位置、FOV、震动）

---

## 九、Mixin 注入系统

### 9.1 录制相关 Mixin

| Mixin                                        | 目标类                                  | 注入点                     | 功能                         |
| -------------------------------------------- | --------------------------------------- | -------------------------- | ---------------------------- |
| `MixinConnection`                            | `Connection`                            | `genericsFtw` (HEAD)       | 截获所有 Packet 加入录制队列 |
|                                              |                                         | `send` (HEAD, cancellable) | 过滤回放时不必要的发包       |
| `MixinClientLevel`                           | `ClientLevel`                           | `setBlock` (HEAD)          | 录制方块预测                 |
|                                              |                                         | `levelEvent` (HEAD)        | 录制世界事件                 |
|                                              |                                         | `playSeededSound` (RETURN) | 录制种子化声音               |
| `MixinLocalPlayer`                           | `LocalPlayer`                           | `tick` / `aiStep` 等       | 录制本地玩家变化             |
| `MixinMultiPlayerGameMode`                   | `MultiPlayerGameMode`                   | 多个方法                   | 录制游戏模式交互             |
| `MixinClientConfigurationPacketListenerImpl` | `ClientConfigurationPacketListenerImpl` | -                          | 配置阶段包录制               |

### 9.2 回放相关 Mixin

| Mixin                        | 目标类                  | 功能                                            |
| ---------------------------- | ----------------------- | ----------------------------------------------- |
| `MixinMinecraft`             | `Minecraft`             | 核心回放逻辑注入                                |
|                              |                         | 覆写 `createIntegratedServer` 返回 ReplayServer |
|                              |                         | 关键帧应用调度                                  |
|                              |                         | 暂停屏幕检测                                    |
| `MixinClientPacketListener`  | `ClientPacketListener`  | 回放时转发自定义包                              |
| `MixinGameRenderer`          | `GameRenderer`          | 渲染帧中的相机关键帧应用                        |
| `MixinCamera`                | `Camera`                | 回放相机控制                                    |
| `MixinEntity`                | `Entity`                | 实体行为修改（如阻止 AI）                       |
| `MixinLivingEntity`          | `LivingEntity`          | 生物行为修改                                    |
| `MixinClientClockManager`    | `ClientClockManager`    | 客户端时间控制                                  |
| `MixinServerTickRateManager` | `ServerTickRateManager` | 服务器 tick rate 控制                           |
| `MixinScoreboard`            | `Scoreboard`            | 记分板修改                                      |
| `MixinMinecraftServer`       | `MinecraftServer`       | 服务器行为覆盖                                  |
| `MixinChunkHolder`           | `ChunkHolder`           | 区块持有修改                                    |
| `MixinLevelChunk`            | `LevelChunk`            | 区块状态直接修改                                |
| `MixinFrustum`               | `Frustum`               | 视锥体裁剪调整                                  |
| `MixinItemInHandRenderer`    | `ItemInHandRenderer`    | 手中物品渲染                                    |

### 9.3 视觉效果 Mixin

| Mixin                | 功能                   |
| -------------------- | ---------------------- |
| `MixinLevelRenderer` | 关卡渲染修改           |
| `MixinWindow`        | 窗口大小跟踪           |
| `MixinRenderTarget`  | 渲染目标修改           |
| `MixinPlayerInfo`    | 玩家信息覆盖（皮肤等） |

---

## 十、自定义网络包

| 包名                              | 方向            | 功能                                 |
| --------------------------------- | --------------- | ------------------------------------ |
| `FinishedServerTick`              | Server → Client | 通知客户端服务器 tick 完成（无数据） |
| `FlashbackForceClientTick`        | Server → Client | 强制客户端执行一次 tick              |
| `FlashbackClearParticles`         | Server → Client | 清除所有粒子                         |
| `FlashbackClearEntities`          | Server → Client | 清除所有非玩家实体                   |
| `FlashbackInstantlyLerp`          | Server → Client | 将所有实体位置插值归零               |
| `FlashbackRemoteSelectHotbarSlot` | Server → Client | 远程设置物品栏选中槽位               |
| `FlashbackRemoteExperience`       | Server → Client | 远程设置经验值                       |
| `FlashbackRemoteFoodData`         | Server → Client | 远程设置饥饿值                       |
| `FlashbackRemoteSetSlot`          | Server → Client | 远程设置物品栏物品                   |
| `FlashbackAccurateEntityPosition` | Server → Client | 精准实体位置（高频更新）             |
| `FlashbackVoiceChatSound`         | Server → Client | VoiceChat 音频数据                   |
| `FlashbackSetBorderLerpStartTime` | Server → Client | 世界边界插值起始时间                 |
| `FlashbackRawCustomPayload`       | Server → Client | 原始 CustomPayload 转发              |

---

## 十一、完整录制流程

```
1. 用户执行 /flashback start（或自动录制触发）
   │
2. Flashback.startRecordingReplay()
   ├── 检查是否已在录制
   ├── 检查不兼容模组
   └── 创建 new Recorder(registryAccess)
       ├── 创建 AsyncReplaySaver → 启动后台写入线程
       ├── 初始化游戏包/配置包编解码器
       ├── 填充 FlashbackMeta 基础信息
       │    ├── dataVersion, protocolVersion, versionString
       │    ├── bobbyWorldName (如有)
       │    ├── distantHorizonPaths (如有)
       │    └── namespacesForRegistries
       └── 赋值 Flashback.RECORDER = this
           │
3. 每个客户端 tick (ClientTickEvents.END_CLIENT_TICK)
   │
   └── Mixin 截获：所有 Packet 经过 MixinConnection.genericsFtw()
       └── Recorder.writePacketAsync(packet, phase)
           ├── 展开 BundlePacket
           ├── 转换 PlayerChat → SystemChat
           ├── 过滤 fabric-screen-handler-api
           ├── 过滤本地玩家自身数据包（由 writeLocalData 处理）
           ├── 过滤 IgnoredPacketSet
           └── 加入 pendingPackets 队列
               │
4. Recorder.endTick()
   ├── [首次] writeSnapshot(true)         ← 完整游戏状态快照
   │   ├── 配置阶段数据
   │   ├── Login + 本地玩家创建
   │   ├── 玩家列表 / Tab / BossBar / 记分板
   │   ├── 世界信息
   │   ├── 所有区块数据
   │   └── 所有实体数据
   │
   ├── flushPackets()                    ← 刷新 pendingPackets
   │   ├── 分 PLAY / CONFIGURATION
   │   ├── 遇到 Login 包时插入 CreateLocalPlayer
   │   └── 跟踪配置阶段结束
   │
   ├── writeEntityPositions()            ← 仅写入变化的实体位置
   │
   ├── writeLocalData()                  ← 写入变化的本地玩家数据
   │   ├── 经验值 / 生命值 / 饥饿值
   │   ├── 选中物品栏槽位
   │   ├── 实体元数据 / 装备
   │   ├── 物品栏内容
   │   ├── 方块破坏进度
   │   ├── 挥臂动画
   │   └── 运动向量
   │
   ├── writeAccurateFirstPersonPosition() ← 可选高帧率位置
   │
   ├── ActionNextTick                    ← 标记 tick 结束
   │
   ├── [每5分钟/chunk边界] writeReplayChunk()
   │   ├── 生成 cN.flashback 文件名
   │   ├── 创建 FlashbackChunkMeta (duration)
   │   ├── 更新 metadata.totalTicks
   │   ├── 序列化 metadata → JSON
   │   ├── AsyncReplaySaver.writeReplayChunk()
   │   └── 写新快照 (writeSnapshot)
   │
   └── [截图] 第 20 tick 或结束时截图
       │
5. 用户执行 /flashback finish
   │
   └── Flashback.finishRecordingReplay()
       ├── recorder.endTickWithContext(true)    ← 关闭标志
       ├── Path recordFolder = recorder.finish()
       └── ReplayExporter.export(recordFolder, outputFile, name)
           ├── 读取 metadata.json
           ├── 验证所有 chunk 文件存在
           ├── 创建 .zip 文件 (Deflater.BEST_SPEED)
           ├── 写入 metadata.json entry
           ├── 写入所有 cN.flashback entry
           ├── 写入 level_chunk_caches/*.bin
           └── 写入 icon.png
```

---

## 十二、完整回放流程

```
1. 用户打开 .zip 回放文件
   │
2. Flashback.openReplay(path)
   ├── 用 FileSystems.newFileSystem(path) 打开 ZIP
   ├── 读取 metadata.json → FlashbackMeta
   ├── 创建临时服务器文件夹 (TempFolderProvider)
   ├── 创建 ReplayServer(...)
   │   ├── 初始化 game/configuration 包处理器
   │   ├── 解析 playableChunksByStart (起始 tick → PlayableChunk)
   │   ├── 创建 ReplayChunkCache
   │   └── 注册 EditorState.usedByPaths
   │
   └── Minecraft 客户端连接 ReplayServer
       │
3. 首个 tick：ReplayServer.tickServer()
   │
   ├── initializedWithSnapshot = true
   ├── 播放初始快照 (chunk 0 的 Snapshot)
   │   ├── replayReader.handleSnapshot(this)
   │   │   ├── 解析所有快照 Action
   │   │   │   ├── ActionConfigurationPacket → 配置阶段数据
   │   │   │   ├── ActionGamePacket → 游戏阶段数据
   │   │   │   │   ├── Login 包 → 设置维度/游戏模式
   │   │   │   │   ├── 玩家信息 → PlayerInfo
   │   │   │   │   ├── 区块数据 → LevelChunk
   │   │   │   │   ├── 实体创建 → Entity spawning
   │   │   │   │   └── BossBar/Tab/Scoreboard 等
   │   │   │   ├── ActionCreateLocalPlayer → 本地玩家实体
   │   │   │   └── ActionMoveEntities → 设置实体位置
   │   │   └── flushPendingEntities() → 真正添加所有实体
   │   └── replayReader.resetToStart() → 重置到 Action 区开头
   │
4. 每个后续 tick
   │
   ├── 更新 replayViewers 列表
   │   ├── 跟踪 spectator 状态
   │   └── 跟踪非 Spectator 观看者
   │
   ├── 应用关键帧 (applyKeyframes)
   │   ├── SpeedKeyframe → 修改 tick rate
   │   ├── FreezeKeyframe → 设置冻结状态
   │   ├── TimeOfDayKeyframe → 修改游戏时间
   │   └── BlockOverrideKeyframe → 方块覆盖
   │
   ├── handleActions()
   │   ├── 判断是否需要 chunk 跳转
   │   │   └── playSnapshot() → 快照重置状态
   │   ├── 获取当前 chunk 的 ReplayReader
   │   └── while (currentTick < targetTick)
   │       ├── applyBlockOverrideKeyframes()
   │       └── replayReader.handleNextAction(this)
   │           ├── ActionNextTick → currentTick++
   │           ├── ActionGamePacket → 解码包 → ReplayGamePacketHandler
   │           │   ├── forward() → 转发给所有 ReplayPlayer
   │           │   ├── 实体创建 → pendingEntities
   │           │   ├── 区块数据 → setBlockStateWithoutUpdates
   │           │   └── BossBar → ReplayServer.updateBossBar()
   │           ├── ActionMoveEntities → 更新实体位置
   │           │   └── Mark needsPositionUpdate
   │           └── ActionLevelChunkCached → 从缓存加载区块
   │
   ├── super.tickServer() → 底层 Minecraft tick
   │
   ├── 广播实体位置更新
   │   └── needsPositionUpdate 中的实体
   │       ├── ClientboundEntityPositionSyncPacket (位置变化)
   │       └── ClientboundMoveEntityPacket.Rot (仅旋转变化)
   │
   ├── 第一人称数据同步
   │   └── for each ReplayPlayer
   │       ├── FlashbackRemoteExperience
   │       ├── FlashbackRemoteFoodData
   │       ├── FlashbackRemoteSelectHotbarSlot
   │       └── FlashbackRemoteSetSlot (x 9)
   │
   └── FinishedServerTick → 通知客户端
       │
5. 编辑器交互（运行时）
   ├── 修改关键帧 → EditorState.dirty = true
   ├── 拖动时间轴 → ReplayServer.goToReplayTick(tick)
   ├── 时间轴跳转 → jumpToTick → playSnapshot(chunk) → 重置状态
   └── 自动保存 → EditorStateManager.saveIfNeeded() (30秒间隔)
```

---

## 十三、导出系统

`ExportJob` 负责将回放渲染为视频文件：

1. 创建离线渲染环境（无 UI）
2. 设置 `PerfectFrames` 模式：每帧精确渲染
3. 逐帧推进 `ReplayServer` 的 tick
4. 使用 FFmpeg (`MixinFFmpegFrameRecorder`) 捕获渲染帧
5. 编码为视频文件

关键配置：
- 分辨率、帧率
- 导出范围（EditorScene.exportStartTicks / exportEndTicks）
- 编码器设置（通过 Lattice 配置系统）

---

## 十四、关键设计要点

### 14.1 基于 Packet 的录制 vs 基于状态的录制

Flashback 采用**混合策略**：
- **网络包录制**：截获所有 Clientbound Packet，保证画面精确还原
- **状态快照**：定期保存完整游戏状态，支持任意位置跳转
- **实体位置单独追踪**：高频写入实体位置变化，避免位置不同步

### 14.2 Chunk 分片机制

- 每 5 分钟（`CHUNK_LENGTH_SECONDS`）创建一个新的 `.flashback` chunk
- 每个 chunk 开头包含一个完整快照
- 跳转时只需播放目标 chunk 的快照即可恢复状态
- Chunk 间无重叠，通过快照实现无缝过渡

### 14.3 异步写入

- `AsyncReplaySaver` 使用单线程消费者模型
- 主线程通过 `submit(Consumer<ReplayWriter>)` 提交写入任务
- 避免磁盘 I/O 阻塞游戏主线程
- 使用 `ArrayBlockingQueue` 实现背压

### 14.4 区块去重缓存

- 相同的 `LevelChunkWithLightPacket` 只存储一次
- 使用 `longHashCode`（基于区块数据的哈希）去重
- 回放时通过 `ActionLevelChunkCached(index)` 引用
- 进一步缓存到 `level_chunk_caches/N.bin` 文件

### 14.5 回放观看者架构

- `ReplayPlayer` 继承 `ServerPlayer`，是"假的"服务器玩家
- 不参与游戏逻辑（无敌、无伤害、无统计）
- 服务器广播包时，仅发给回放观看者而非所有玩家
- `broadcast()` 方法被覆写以按距离过滤

---

## 十五、类关系图

```
┌─────────────────────────────────────────────────────────────────┐
│                        Flashback.java                           │
│  (ModInitializer, ClientModInitializer)                         │
│  - RECORDER: Recorder                                           │
│  - EXPORT_JOB: ExportJob                                        │
│  - 命令注册 / 事件注册 / 配置管理                                  │
└──────┬──────────────────────┬──────────────────────┬────────────┘
       │                      │                      │
       ▼                      ▼                      ▼
┌──────────────┐    ┌──────────────────┐    ┌──────────────────┐
│   Recorder   │    │   ReplayServer   │    │   ExportJob      │
│  (录制引擎)   │    │   (回放引擎)      │    │   (导出引擎)      │
└──────┬───────┘    └────────┬─────────┘    └──────────────────┘
       │                     │
       ▼                     ▼
┌──────────────┐    ┌──────────────────┐
│AsyncReplay   │    │  ReplayReader    │
│   Saver      │    │  (读取 .flashback)│
│  (异步写入)   │    └────────┬─────────┘
└──────┬───────┘             │
       │                     ▼
       ▼            ┌──────────────────┐
┌──────────────┐    │  Action 系统      │
│ ReplayWriter │    │  ActionNextTick   │
│  (二进制格式) │    │  ActionGamePacket │
└──────────────┘    │  ActionMoveEntities│
                    │  ...              │
                    └──────────────────┘
```