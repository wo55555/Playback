# 06 · 数据模型与持久化

> 入口：`src/playback/refactor/data/`
> 角色：定义**所有新增**的数据结构与持久化格式。包含 `.playback` 的 `editor` 节点、布局 / 工作区 / 快捷键 / 录制预设 / 编辑器偏好。
> **视频编辑工作流**遵循 [09-video-editing-workflow.md](09-video-editing-workflow.md) 的 3 条一级轨道模型：
> - **数据模型已切换为**：`SequenceSegment` + `WorldActorSegment` + `CameraEntity` + `SubActor` + `WorldActor`（取代旧 `Track / Clip / TrackKind / Transition / CameraTrackExt`）。
> - 旧 `TrackKind::Video / Camera / Marker` 与 `Clip / Track / Transition` **整体下线**（参见 [04 §VE-9](04-video-editing.md)）。
> 与旧文档的关系：摄影机轨道基础（关键帧 / easing）见 [editor/camera-track.md](../editor/camera-track.md)；本文件**只描述新增**。

## 一、需求（Requirements）

### 1.1 功能性需求

| ID | 需求 | 优先级 |
|---|---|---|
| DP-1 | `.playback` ZIP 内 `metadata.json` 增 `editor` 节点（含 `sequence` / `worldActor` / `cameras` 三大条目） | P0 |
| DP-2 | 编辑器布局存 `data/editor/layouts/<name>.json`（用户可命名 / 导入导出） | P0 |
| DP-3 | 工作区存 `data/editor/workspaces/<name>.json`（Edit / Export 内置 + 用户） | P0 |
| DP-4 | 快捷键存 `data/editor/keymap.json`（可绑定 / 可序列化） | P0 |
| DP-5 | 录制配置预设存 `data/recording/profiles/<name>.json` | P0 |
| DP-6 | 编辑器偏好（主题 / 默认布局）存 `data/editor/preferences.json` | P0 |
| DP-7 | 所有 JSON 格式带 `version` 字段；缺字段时回退默认 | P0 |
| DP-8 | 序列化 / 反序列化**对称**：round-trip 字段值不变 | P0 |
| DP-9 | nlohmann::json 序列化（与现有项目一致） | P0 |
| DP-10 | 旧 `.playback`（无 `editor` 节点或使用旧 `tracks` / `videoTracks` / `cameraTracks` 字段）加载时按新模型**重建** | P0 |

### 1.2 非功能性需求

- **可演进**：每次结构变更 `version++`；旧 `version` 文件加载时按需迁移
- **可读性**：JSON 字段用 snake_case，结构嵌套 ≤ 3 层
- **可校验**：每个根结构带 `validate()` 函数，返回 `vector<ValidationError>`
- **零破坏**：旧 `.playback` 文件（无 `editor` 节点）必须能正常加载

### 1.3 与现有约束对齐

- 复用 [PlaybackMeta](file:///d:/raplay/Playback/src/playback/functions/record/Recorder.h#L38)（在 `Recorder.h`）的 `nlohmann::json` 序列化路径
- 复用 [EditorContext](file:///d:/raplay/Playback/src/playback/editor/context/EditorContext.h) 的 mutex 模式
- **不入**新依赖（无 protobuf / flatbuffers / sqlite）

## 二、架构（Architecture）

### 2.1 内部结构

```
refactor/data/
├── SequenceSegment.h              ← 摄像机序列段（[09 §2.2](09-video-editing-workflow.md)）
├── WorldActorSegment.h            ← 世界Actor 段（[09 §2.2](09-video-editing-workflow.md)）
├── CameraEntity.h                 ← 摄影机（4 种 kind；[09 §2.2](09-video-editing-workflow.md)）
├── SubActor.h                     ← 子Actor（4 类别 + agentDetails）
├── WorldActor.h                   ← 容器：解析自 .playback
├── CameraPath.h                   ← 3D 样条路径
├── CameraRig.h                    ← 摄影机运动原语状态
├── CameraPreset.h                 ← 摄影机预设定义
├── CameraShake.h                  ← 摄影机抖动（噪声）
├── CameraLimiter.h                ← 限位器（AABB）
├── TimeRemap.h                    ← 时间重映射（速度曲线，序列段内 speed 的低阶替代）
├── Marker.h                       ← 章节标记
├── BezierCurve.h                  ← 通用贝塞尔曲线
├── EditorLayout.h                 ← 编辑器布局
├── EditorWorkspace.h              ← 工作区
├── KeyBinding.h                   ← 快捷键绑定
├── RecordingProfile.h             ← 录制配置预设
├── EditorPreferences.h            ← 编辑器偏好
├── JsonCodec.h / .cpp             ← 通用 JSON 编 / 解码（模板特化）
└── Migration.h / .cpp             ← 版本迁移（v1 → v2 → v3 ...）
```

### 2.2 `.playback` 扩展（PlaybackMeta 增字段）

在 [PlaybackMeta](file:///d:/raplay/Playback/src/playback/functions/record/Recorder.h#L38) 增加：

```cpp
struct PlaybackMeta {
    // ... 旧字段 ...
    std::string name;
    std::string worldName;
    int         duration{};
    int         totalTicks{};
    PlaybackView initialView{};
    LinkedHashMap<std::string, ChunkMeta> chunks;

    // ===== 新增 =====
    std::optional<EditorState> editor;     // 见 §2.3
};
```

`toJson` / `fromJson` 增加 `editor` 节点；缺字段时 `editor = std::nullopt`，旧回放文件**不破坏**。

### 2.3 `EditorStateExt`（`.playback` editor 节点 · 3 条一级轨道模型）

```cpp
// EditorStateExt.h
struct EditorStateExt {
    int                       version{3};            // editor 节点版本（v3 = 3 条一级轨道）
    int                       currentTick{};
    int                       totalTicks{};          // = WorldActor.totalTicks
    bool                      playing{};
    float                     playbackSpeed{1.0f};

    // ====== 摄像机序列（顶轨） ======
    std::vector<SequenceSegment> sequence;           // 1..256 段；默认 1 段 [0, totalTicks]
    // 注意：序列本身**不存 Camera**，仅存 `cameraId` 引用 → cameras[]

    // ====== 世界Actor（中轨） ======
    WorldActor                  worldActor;          // 1 个；内含 segments + subActors

    // ====== 摄像机（底轨 0..N） ======
    std::vector<CameraEntity>   cameras;            // 0..16 台；按用户添加顺序
    int                         activeCameraIndex{}; // 当前激活（gizmo 高亮 / Details 上下文）

    // ====== 独立轨（保留） ======
    std::vector<Marker>         markers;            // 标记轨；与条目无关

    // ====== 性能 ======
    float                       fps{60.0f};
    size_t                      memoryUsageBytes{};
};
```

> **删除项（与 v1/v2 不再兼容）**：
> - `EditorState.tracks`（旧 `CameraTrackExt[]` 数组）→ 由 `cameras` 替代
> - `EditorState.videoTracks`（旧 `Track[]` 数组）→ 由 `sequence` 替代
> - `EditorState.transitions`（旧转场）→ **整体删除**（[04 §VE-9](04-video-editing.md)）
> - `EditorState.timeRemap`（全局变速）→ 由 `SequenceSegment.speed` 替代；本期下线
> - `EditorState.curves`（共享贝塞尔曲线）→ 嵌入 `CameraKeyframe.easing` / `RigSegment.easing`
> - `EditorState.activeCameraTrackIdx` → 由 `activeCameraIndex` 替代
> - `EditorState.activeVideoTrackIdx` → 不再需要
> - `EditorState.playheadTick` → 由 `currentTick` 替代
>
> **旧字段保留（兼容层）**：v2 文件反序列化时，旧 `tracks[]` 数组的每个元素先尝试 `CameraTrackExt` → `CameraEntity` 字段映射；旧 `videoTracks[]` 与 `transitions` 字段**忽略**（不进入新模型），旧文件加载后 sequence 默认为 1 段、cameras 默认为空、WorldActor 段默认为 1 段。

### 2.3.1 `SequenceSegment`（序列段）

```cpp
struct SequenceSegment {
    std::string id;                  // uuid
    int         startTick{};         // 在时间轴上的起点
    int         endTick{};           // 在时间轴上的终点
    int         sourceTick{};        // 对应世界Actor 的源 tick（默认 = startTick）
    std::string cameraId;            // 绑定的 Camera id；空 = 列表中第 1 台
    float       speed{1.0f};         // 段内播放速度（影响 sourceTick 步进）
    Color4      color{0.20f, 0.55f, 0.95f, 1.0f};  // 默认蓝
    bool        locked{};
};
```

序列化（示例）：

```json
{
  "id": "seq-uuid-001",
  "startTick": 0,
  "endTick": 6000,
  "sourceTick": 0,
  "cameraId": "cam-uuid-001",
  "speed": 1.0,
  "color": "#338cf0",
  "locked": false
}
```

### 2.3.2 `WorldActor` + `WorldActorSegment`（世界Actor 容器）

```cpp
struct WorldActor {
    std::string                     id;              // 与 .playback 文件同 id
    std::string                     name;            // 回放名
    int                             totalTicks{};    // 总时长
    std::vector<WorldActorSegment>  segments;        // 段（默认 1 段 = 整条）
    std::vector<SubActor>           subActors;       // 解析出的子Actor（按 .playback 解析顺序）
};

struct WorldActorSegment {
    std::string id;                  // uuid
    int         startTick{};         // 在时间轴上的起点
    int         endTick{};           // 在时间轴上的终点
    int         sourceTick{};        // 对应回放源 tick
    float       speed{1.0f};         // 段内播放速度
    Color4      color{0.95f, 0.55f, 0.20f, 1.0f};  // 默认橙
    bool        locked{};
};
```

### 2.3.3 `CameraEntity`（摄影机）

```cpp
struct CameraEntity {
    std::string id;                  // uuid
    std::string name;                // 人类可读
    CameraKind  kind{CameraKind::Keyframe};
    std::vector<CameraKeyframe>  keys;             // kind=Keyframe 用
    std::optional<CameraPath>    path;             // kind=Path 用
    std::optional<CameraRig>     rig;              // kind=Rig 用
    std::optional<CameraPreset>  preset;           // kind=Preset 用
    std::optional<CameraShake>   shake;            // 任意 kind 可叠
    std::optional<CameraLimiter> limiter;          // 限位
    std::string  bindingEntityUuid;                 // 绑定的子Actor；空 = 自由机位
    int          bindingMode{0};                    // 0=无 1=位置 2=角度 3=全
    float        bindingDamping{0.1f};
    bool         active{false};                     // 当前激活（gizmo / Details 上下文）
    bool         locked{};
};
```

> **取代旧 `CameraTrackExt`**：旧结构位于 `EditorState.tracks[]`；新结构位于 `EditorState.cameras[]`。
> 字段含义完全兼容；新增 `active` / `locked` 字段（缺省 = false / false）。

### 2.3.4 `SubActor`（子Actor · 解析自 .playback）

```cpp
enum class SubActorCategory : uint8_t {
    Default = 0,
    Players,
    Creatures,
    Entities
};

struct SubActor {
    std::string       id;            // uuid（来自 .playback）
    std::string       name;
    SubActorCategory  category{SubActorCategory::Default};
    Vec3              position{};
    Vec2              rotation{};
    nlohmann::json    agentDetails;  // 自由结构，按 category 决定 key
    std::string       boundCameraId; // 若被一台 Camera 绑定，则非空
};
```

> **关键不变量**：`SubActorCategory` 不可改（解析自 .playback；UI 只读 category）。
> `agentDetails` 字段 schema 按 category 决定：
> - **Default**：基础 (health / hunger / armor)
> - **Players**：Default + (gamemode / dimension / inventory)
> - **Creatures**：Default + (species / age / tameStatus)
> - **Entities**：Default + (entityType / flags)
> 具体 schema 由 [03-advanced-recording.md](03-advanced-recording.md) 的 packet 解析器决定。

### 2.4 `Marker`（章节标记）

```cpp
struct Marker {
    std::string name;
    int         tick{};
    Color4      color{Color4::Yellow};
    std::string note;                       // 可选描述（i18n key）
};
```

序列化：

```json
{
  "name": "Boss Spawn",
  "tick": 2400,
  "color": "#FFD700",
  "note": "playback.editor.marker.bossSpawn"
}
```

### 2.5 `CameraPath`（3D 样条路径）

```cpp
enum class SplineType : uint8_t {
    Linear = 0,
    CatmullRom,       // 3 次样条，过控制点
    CubicBezier       // 3 次贝塞尔，每段独立控制点
};

struct SplineControlPoint {
    Vec3         position;
    Vec3         inTangent;        // Bezier 用；Catmull-Rom 自动算
    Vec3         outTangent;
    int          tick{};           // 该点在时间轴上的位置
    EasingType   easing{EasingType::Linear};
};

struct CameraPath {
    SplineType                       type{SplineType::CatmullRom};
    std::vector<SplineControlPoint>  points;        // 升序 tick
    bool                             closed{false}; // 闭环（Orbit 用）
    float                            defaultFov{90.0f};
    Vec2                             defaultRotation{0, 0};
};
```

**采样**：

```cpp
CameraSample sampleAt(const CameraPath& path, int tick);
// 1) locateSegment(tick) 同 CameraTrack
// 2) 按 type 插值：
//    - Linear: 直线 lerp
//    - CatmullRom: t ∈ [0,1]，位置 = 4 点（p0,p1,p2,p3）的 0.5 * ((-p0+3p1-3p2+p3)t³ + (2p0-5p1+4p2-p3)t² + (-p0+p2)t + 2p1)
//    - CubicBezier: De Casteljau
// 3) tangent → 旋转（look-at 下一段方向）
```

### 2.6 `CameraRig`（8 种运动原语）

```cpp
enum class RigMotion : uint8_t {
    None = 0,
    Dolly,        // 前后推拉（沿摄影机前向轴）
    Truck,        // 左右平移
    Pedestal,     // 上下平移
    Pan,          // 水平旋转（绕 Y 轴）
    Tilt,         // 俯仰（绕 X 轴）
    Roll,         // 滚转（绕前向轴）
    Zoom,         // FOV 缩放
    Follow        // 跟随目标
};

struct RigSegment {
    RigMotion     motion{RigMotion::None};
    int           startTick{};
    int           endTick{};
    float         startValue{0};
    float         endValue{0};
    EasingType    easing{EasingType::EaseInOut};
};

struct CameraRig {
    std::vector<RigSegment> segments;
    Vec3                    basePosition;
    Vec2                    baseRotation;
    float                   baseFov{90.0f};
};
```

**采样**：对每个激活的 motion（`tick` ∈ `[startTick, endTick]`）独立插值，叠加到 base。

### 2.7 `CameraPreset`（7 种摄影机预设）

```cpp
enum class PresetKind : uint8_t {
    FirstPerson = 0,   // 第一人称：玩家视角
    ThirdPerson,       // 第三人称：玩家后上方
    Free,               // 自由机位
    FollowEntity,       // 跟随实体（需 bindingEntityUuid）
    Orbit,              // 环绕：绕中心点
    Telephoto,          // 长焦：窄 FOV
    Drone               // 无人机：鸟瞰 + 自由
};

struct CameraPreset {
    PresetKind kind{PresetKind::Free};
    float      fov{90.0f};
    Vec3       offset{0, 0, 0};      // 相对锚点的偏移
    Vec2       rotation{0, 0};
    float      orbitRadius{10.0f};
    float      orbitSpeed{1.0f};
    Vec3       orbitCenter{0, 0, 0};
    int        orbitPhaseTick{0};
    float      damping{0.1f};
};
```

### 2.8 `CameraShake`（抖动）

```cpp
enum class ShakeNoise : uint8_t { Perlin = 0, Simplex };

struct CameraShake {
    int         startTick{};
    int         endTick{};
    float       amplitude{1.0f};        // 世界单位 / 度
    float       frequency{10.0f};       // Hz
    ShakeNoise  noise{ShakeNoise::Perlin};
    Vec3        positionSeed{0, 0, 0}; // 噪声种子
    Vec3        rotationSeed{0, 0, 0};
    float       decay{EasingType::EaseOut}; // 末端衰减
};
```

**采样**（伪代码）：

```cpp
Vec3 shakeOffsetAt(const CameraShake& s, int tick, int seed) {
    float t = (tick - s.startTick) / float(s.endTick - s.startTick);
    if (t < 0 || t > 1) return {};
    float decay = easingValue(s.decay, t);  // 末端衰减
    return Vec3{
        perlin(s.positionSeed.x + tick * s.frequency * 0.01f) * s.amplitude * decay,
        perlin(s.positionSeed.y + tick * s.frequency * 0.01f) * s.amplitude * decay,
        perlin(s.positionSeed.z + tick * s.frequency * 0.01f) * s.amplitude * decay
    };
}
```

> Perlin/Simplex 由 [stb_perlin.h](https://github.com/nothings/stb) 提供，**单头**，可直接 include（与 PNG 序列共用 stb）。

### 2.9 `CameraLimiter`（限位器）

```cpp
enum class LimiterShape : uint8_t { Box = 0, Sphere, PlayerAABB };

struct CameraLimiter {
    LimiterShape  shape{LimiterShape::Box};
    AABB          box{{-100,0,-100}, {100,256,100}};   // Box 用
    Vec3          sphereCenter{0, 80, 0};
    float         sphereRadius{200.0f};
    bool          followPlayer{true};                  // PlayerAABB 跟着玩家移动
    Vec3          padding{0, 0, 0};                    // 距限位面 padding
};
```

**算法**：采样后 `clamp(cameraPosition, limiter)`。

### 2.10 `TimeRemap`（时间重映射 · **本期下线**）

> **新工作流**：[09 §1.2](09-video-editing-workflow.md) 不再使用全局 TimeRemap。变速通过 `SequenceSegment.speed` / `WorldActorSegment.speed` 实现。
> 本节保留数据结构定义，仅供未来扩展或回退使用；新 `EditorStateExt` **不**序列化 `timeRemap` 字段。

```cpp
struct TimeRemap {
    std::vector<RemapPoint> points;   // 升序 tick
};

struct RemapPoint {
    int   sourceTick{};     // 原始时间轴
    int   targetTick{};     // 重映射后时间轴
    EasingType easing{EasingType::Linear};
};

// 原始 tick → 渲染 tick
int TimeRemap::remap(int sourceTick) const {
    if (points.empty()) return sourceTick;
    if (sourceTick <= points.front().sourceTick) return points.front().targetTick;
    if (sourceTick >= points.back().sourceTick)  return points.back().targetTick;
    // locateSegment + 插值
}
```

### 2.11 ~~`Clip` / `Track` / `Transition`~~（整体下线）

> **新工作流 [04 §VE-9](04-video-editing.md) 已明确下线**：
> - `Clip`（视频剪辑）→ 由 `SequenceSegment` + `WorldActorSegment` 替代
> - `Track`（时间轴轨道 + `TrackKind`）→ 由 3 条一级轨道（sequence / worldActor / cameras）替代
> - `Transition`（Cut / Fade / CrossDissolve）→ 由"序列段切换 Camera"实现（**无转场概念**）
> - `TrackManager` / `TransitionEngine` 旧 API → 由 `SequenceOps` / `CameraBindingOps` 替代
>
> **代码层**：`src/playback/refactor/editor/models/Track.h` 后续清理时移除 `TrackKind` enum 与 `Clip / Track / Transition` 结构体；新代码不再引用。
>
> **JSON 层**：旧 `videoTracks` / `transitions` 字段加载时被忽略；新文件不写这些字段。

### 2.12 `BezierCurve`（通用曲线 · 关键帧 easing 共用）

```cpp
struct BezierPoint {
    float t{0};             // x ∈ [0,1]
    float v{0};             // y
    Vec2  inTangent{0,0};   // 相对控制点偏移
    Vec2  outTangent{0,0};
};

struct BezierCurve {
    std::string             name;
    std::vector<BezierPoint> points;
    bool                    loop{false};

    // 任意 t (0..1) 采样 y
    float sample(float t) const;
};
```

**用途**（新工作流）：
- `CameraKeyframe.easing`（Keyframe 段间）
- `RigSegment.easing`（运动原语）
- `SequenceSegment.speed` 切换曲线（未来扩展）

> 旧工作流把 `BezierCurve` 独立存储在 `EditorState.curves[]` 中；新工作流**直接嵌入** easing 字段，不再有 `curves[]` 数组。

### 2.13 `EditorLayout`（布局文件）

```cpp
// data/editor/layouts/<name>.json
struct EditorLayout {
    int                       version{1};
    std::string               name;
    DockLayout                docks;       // ImGui DockBuilder 序列化
    std::vector<WindowState>  windows;     // 弹出去的 OS 窗口
    std::vector<PanelState>   panels;      // 面板特定状态（折叠 / 选中）
    std::string               activeWorkspace;
    std::string               activeLayoutName;  // 自身引用
};
```

`DockLayout` 是 ImGui `DockBuilder` 节点的 JSON 树（**用 `imgui.ini` dump 出来直接序列化**；不发明新格式）。

### 2.14 `EditorWorkspace`（工作区文件）

```cpp
// data/editor/workspaces/<name>.json
struct EditorWorkspace {
    int                          version{1};
    std::string                  name;
    std::string                  displayName;   // i18n key
    std::vector<std::string>     visiblePanels; // 该工作区默认显示的面板
    std::string                  defaultLayout; // 关联布局
    std::map<std::string, std::string> panelSettings; // 面板 → 字符串设置
};

// 内置：Edit / Export
// 用户：可创建 / 复制 / 重命名
```

### 2.15 `KeyBinding`（快捷键）

```cpp
// data/editor/keymap.json
struct KeyBinding {
    std::string      action;       // e.g. "playback.togglePause"
    ImGuiKey         key{ImGuiKey_None};
    bool             ctrl{false};
    bool             shift{false};
    bool             alt{false};
};

struct KeyMap {
    int                     version{1};
    std::vector<KeyBinding> bindings;  // 按 action 索引

    // 查：action → keys
    std::vector<KeyBinding> findBindings(std::string_view action) const;
    // 查：keys → action（用于显示）
    std::string             findAction(ImGuiKey k, bool ctrl, bool shift, bool alt) const;
};
```

### 2.16 `RecordingProfile`（录制预设）

```cpp
// data/recording/profiles/<name>.json
struct RecordingProfile {
    int             version{1};
    std::string     name;
    int             recordChunkTicks{20*60*5};     // 5 分钟
    bool            captureInitialSnapshot{true};
    int             snapshotIntervalTicks{0};      // 0=不主动 capture
    bool            recordAudio{false};            // 预留（本期不实现）
    std::vector<std::string> enabledPackets;        // 过滤白名单；空 = 全开
    std::vector<std::string> disabledPackets;       // 过滤黑名单
    bool            showHud{true};                  // 关联 B3
    int             hudPosition{0};                 // 0=左上 1=右上 2=左下 3=右下
    Hotkey          liveMarkerHotkey;               // 关联 B2
    int             autoExportOnStop{0};            // 0=否 1=自动进队列（关联 B6，**本期保留字段不开**）
};
```

**关联 03-advanced-recording.md**。

### 2.17 `EditorPreferences`（全局偏好）

```cpp
// data/editor/preferences.json
struct EditorPreferences {
    int                      version{1};
    std::string              theme{"dark"};        // dark / light / high-contrast
    std::string              activeLayout{"default"};
    std::string              activeWorkspace{"Edit"};
    std::string              language{"system"};   // system / en_US / zh_CN
    bool                     showFps{true};
    int                      autosaveSeconds{30};  // 0=关
    int                      maxUndoSteps{100};
    bool                     developerMode{false}; // 显示隐藏面板
    int                      previewResolution{540}; // 预览视口高度（p）
};
```

### 2.18 持久化总目录

```
data/
├── editor/
│   ├── preferences.json                ← 全局偏好
│   ├── keymap.json                     ← 快捷键
│   ├── layouts/
│   │   ├── default.json                ← 内置默认布局
│   │   ├── color.json                  ← 调色布局
│   │   ├── export.json                 ← 导出布局
│   │   └── <user-name>.json
│   └── workspaces/
│       ├── Edit.json                   ← 内置
│       ├── Export.json                 ← 内置
│       └── <user-name>.json
├── recording/
│   └── profiles/
│       ├── Balanced.json               ← 内置
│       ├── HighQuality.json            ← 内置
│       ├── SpaceSaving.json            ← 内置
│       └── <user-name>.json
└── exports/
    └── presets/                        ← 已有（用户保存的导出预设）
```

内置文件在 `resources/defaults/` 下作为"出厂配置"；用户首次启动拷贝到 `data/`。

### 2.19 版本迁移

```cpp
// Migration.h
class MigrationChain {
public:
    template<typename T>
    T migrate(const nlohmann::json& j) const {
        int v = j.value("version", 1);
        nlohmann::json current = j;
        for (auto& step : steps_) {
            if (v < step.toVersion) {
                current = step.migrate(current);
                v = step.toVersion;
            }
        }
        return T::fromJson(current);
    }
private:
    struct Step { int toVersion; std::function<nlohmann::json(const nlohmann::json&)> migrate; };
    std::vector<Step> steps_;
};
```

**v1 → v2 已规划**：
- v1 只有 `cameraTracks`（旧字段）
- v2 把 `cameraTracks` 改名为 `editor.tracks`；原 `CameraKeyframe` 加 `positionSeed` 字段（兼容：缺则用 0）

**v2 → v3（新工作流）**：
- v2 的 `EditorState.tracks[]`（`CameraTrackExt[]`） → v3 的 `EditorState.cameras[]`（`CameraEntity[]`）
  - 每个 `CameraTrackExt` → `CameraEntity`：字段一一映射，`override` 字段丢弃
- v2 的 `EditorState.videoTracks[]`（`Track[]`）→ v3 的 `EditorState.sequence[]`（`SequenceSegment[]`）：
  - 默认 1 段 `[0, totalTicks]`，`cameraId` 来自原 `clip.activeCameraTrackIdx` 指向的 track id
  - 旧 track 中每个 `clip` 的 `speed` 字段被丢弃（由用户后续手动设置）
- v2 的 `EditorState.transitions[]` → v3 整体删除
- v2 的 `EditorState.timeRemap` → v3 整体删除
- v2 的 `EditorState.curves[]` → v3 整体删除（easing 直接嵌入）
- v2 的 `EditorState.activeCameraTrackIdx` → v3 的 `EditorState.activeCameraIndex`
- v2 的 `EditorState.activeVideoTrackIdx` → v3 删除
- v2 的 `EditorState.playheadTick` → v3 的 `EditorState.currentTick`

**v3 的初始默认值**（v2 旧字段缺省时）：
- `sequence` = `[ { id: genUuid, startTick: 0, endTick: totalTicks, sourceTick: 0, cameraId: "", speed: 1.0, ... } ]`（1 段，未绑）
- `worldActor.segments` = `[ { id: genUuid, startTick: 0, endTick: totalTicks, sourceTick: 0, speed: 1.0, ... } ]`（1 段）
- `cameras` = `[]`（空）

### 2.20 通用编 / 解码（JsonCodec）

```cpp
// JsonCodec.h
template<typename T>
nlohmann::json toJson(const T& v);   // 通过 ADL 找 T::toJson() 或 nlohmann::adl_serializer

template<typename T>
T fromJson(const nlohmann::json& j);  // 通过 T::fromJson() 或 nlohmann::adl_serializer

// 校验
template<typename T>
std::vector<ValidationError> validateJson(const nlohmann::json& j);
```

每个结构**强制**实现 `toJson` / `fromJson` / `validate` 三个方法。`nlohmann::json` 默认不导出 enum，自定义 helper：

```cpp
template<typename E>
nlohmann::json enumToJson(E e) { return static_cast<int>(e); }
template<typename E>
E enumFromJson(const nlohmann::json& j, E def) { return j.is_number_integer() ? static_cast<E>(j.get<int>()) : def; }
```

## 三、执行（Execution）

### 3.1 任务拆分

| 步骤 | 文件 | 验证 |
|---|---|---|
| 1 | `JsonCodec.h` 通用模板 | 单测：每种类型 round-trip |
| 2 | `Vec3` / `Vec2` / `Color4` / `AABB` 的 nlohmann 特化 | 编译 |
| 3 | `SequenceSegment` / `WorldActorSegment` / `WorldActor` + 序列化 | 单测：段覆盖 / split / merge |
| 4 | `CameraEntity` + 序列化（4 种 kind + 绑定 + Shake + Limiter） | 单测：JSON round-trip |
| 5 | `SubActor` + 4 类别枚举 + 序列化 | 单测：4 类别各 1 个 round-trip |
| 6 | `CameraPath` / `CameraRig` / `CameraPreset` / `CameraShake` / `CameraLimiter` 数据 + 序列化 | 单测：locate / sampleAt |
| 7 | `Marker` / `BezierCurve` | 单测：remap 边界 / 5 个采样点 |
| 8 | `EditorStateExt` v3 重构（移除旧字段；新增 sequence / worldActor / cameras） | 编译 + 旧数据回退 |
| 9 | `EditorLayout`（含 DockLayout 序列化） | 手动：保存 → 加载 → 布局恢复 |
| 10 | `EditorWorkspace` | 单测：可见面板列表 |
| 11 | `KeyBinding` / `KeyMap` | 单测：双向查找 |
| 12 | `RecordingProfile` | 单测：JSON round-trip |
| 13 | `EditorPreferences` | 单测：JSON round-trip |
| 14 | `MigrationChain` v1→v2→v3 | 单测：旧文件加载正确迁移 |
| 15 | `PlaybackMeta` 增 `editor` 字段 | 手动：旧 .playback 加载不报错 |
| 16 | 首次启动拷贝内置文件 | 手动：删 data/ → 启动 → 看到 default |
| 17 | 移除旧 `Track.h` 中的 `TrackKind` / `Clip` / `Track` / `Transition`（代码清理） | 编译通过 |

### 3.2 关键不变量

1. **JSON 字段 snake_case**：与现有项目风格一致。
2. **缺字段不报错**：每个 `fromJson` 必须给默认值。
3. **version 永远 int**：从 1 开始递增；旧文件 version 缺则按 1 处理。
4. **迁移幂等**：对 v2 文件跑 v1→v2 迁移，结果不变。
5. **数据可克隆**：所有结构 `T clone() const` 浅拷贝（避免 `shared_ptr` 传染）。
6. **线程安全**：`validate()` 是 const + 纯函数，可多线程并发。

### 3.3 测试用例

| ID | 用例 | 期望 |
|---|---|---|
| DP-T1 | `EditorStateExt` round-trip | 所有字段不变（v3 schema） |
| DP-T2 | 旧 `.playback`（无 editor 节点）加载 | `editor = nullopt`，加载后再写 = 1 段默认 sequence |
| DP-T3 | v1 → v2 → v3 迁移 | 字段全映射；旧 tracks→cameras、videoTracks→sequence |
| DP-T4 | `fromJson` 缺字段 | 缺字段用默认 |
| DP-T5 | `BezierCurve.sample(0.5)` 线性 | y = (p0.v + p1.v) / 2 |
| DP-T6 | `SequenceSegment` round-trip | 字段一致 |
| DP-T7 | `WorldActor` round-trip | segments + subActors 完整保留 |
| DP-T8 | `SubActor` 4 类别各 1 个 round-trip | 类别 enum 保留 |
| DP-T9 | `CameraEntity` 4 种 kind 各 1 个 round-trip | keys / path / rig / preset 不混淆 |
| DP-T10 | `validate` 命中不合法 fps | 含 E_FpsOutOfRange |
| DP-T11 | `EditorLayout` 序列化 | ImGui DockBuilder 可还原 |
| DP-T12 | `KeyMap` 双向查找 | ctrl+space → playback.togglePause |
| DP-T13 | `RecordingProfile` round-trip | 字段全一致 |
| DP-T14 | 旧 v2 文件加载（无 cameras / sequence 字段） | 重建：1 段 sequence、1 段 worldActor、cameras 空 |

### 3.4 风险与回退

| 风险 | 缓解 |
|---|---|
| ImGui DockBuilder 序列化格式变化 | 用 imgui 自己的 `SaveIniSettingsToMemory` dump，不发明新格式 |
| 旧 .playback 加载失败 | `editor = nullopt` 兜底；UI 提示"该回放无编辑器数据" |
| `stb_perlin.h` 性能 | 抖动计算缓存（每 5 tick 一次） |
| JSON 体积过大（多轨 + 多 Clip） | gzip 压缩（可选）；首期不压 |
| `nlohmann::json` 反射枚举 | 显式 helper；不依赖 magic_get |

## 四、模块关系

### 被谁调用（上游）

- **`refactor/camera-motion/*`**：用 `CameraEntity` / `CameraPath` / `CameraRig` / `CameraShake` / `CameraLimiter` / `CameraPreset`
- **`refactor/video-editing/*`**：用 `SequenceSegment` / `WorldActorSegment` / `WorldActor` / `CameraEntity` / `SubActor` + `SequenceOps` / `CameraBindingOps`（取代旧 `Clip / Track / Transition`）
- **`refactor/advanced-recording/*`**：用 `RecordingProfile`
- **`refactor/render-pipeline/*`**：用 `EditorStateExt`（sequence / worldActor / cameras）渲染
- **`refactor/editor/*`**：用 `EditorLayout` / `EditorWorkspace` / `KeyMap` / `EditorPreferences`

### 调用谁（下游）

- **nlohmann::json**：序列化
- **stb_perlin.h**（新增）：抖动噪声
- **ImGui `DockBuilder`**：布局序列化
- **旧 [EditorContext](../editor/context/EditorContext.md)**：通过 [EditorBridge](01-editor-architecture.md) 读写 `EditorStateExt`

### 共享数据

- `EditorStateExt` 单例：UI / Domain / Render 共读
- 旧 `EditorContext::mCameraExt` / `mEditorExt` / `mExportExt` 由 `EditorBridge` 同步

### 事件订阅 / 发送

- `EditorStateExt.onChanged`（任何字段变化）→ `EditorBridge.sync()` → 旧 `EditorContext`

## 五、阅读顺序

1. 本文件
2. [09-video-editing-workflow.md](09-video-editing-workflow.md) —— 工作流总览（必读；确定数据模型前提）
3. [02-camera-motion.md](02-camera-motion.md) —— 摄影机算法
4. [04-video-editing.md](04-video-editing.md) —— 剪辑操作
5. [03-advanced-recording.md](03-advanced-recording.md) —— 录制（决定 SubActor 解析）
6. [05-render-pipeline.md](05-render-pipeline.md) —— 渲染消费
7. [01-editor-architecture.md](01-editor-architecture.md) —— UI
