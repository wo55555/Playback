# 06 · 数据模型与持久化

> 入口：`src/playback/refactor/data/`
> 角色：定义**所有新增**的数据结构与持久化格式。包含 `.playback` 的 `editor` 节点、布局 / 工作区 / 快捷键 / 录制预设 / 编辑器偏好。
> 与旧文档的关系：摄影机轨道基础（关键帧 / easing）见 [editor/camera-track.md](../editor/camera-track.md)；本文件**只描述新增**。

## 一、需求（Requirements）

### 1.1 功能性需求

| ID | 需求 | 优先级 |
|---|---|---|
| DP-1 | `.playback` ZIP 内 `metadata.json` 增 `editor` 节点（含全部编辑状态） | P0 |
| DP-2 | 编辑器布局存 `data/editor/layouts/<name>.json`（用户可命名 / 导入导出） | P0 |
| DP-3 | 工作区存 `data/editor/workspaces/<name>.json`（Edit / Export 内置 + 用户） | P0 |
| DP-4 | 快捷键存 `data/editor/keymap.json`（可绑定 / 可序列化） | P0 |
| DP-5 | 录制配置预设存 `data/recording/profiles/<name>.json` | P0 |
| DP-6 | 编辑器偏好（主题 / 默认布局）存 `data/editor/preferences.json` | P0 |
| DP-7 | 所有 JSON 格式带 `version` 字段；缺字段时回退默认 | P0 |
| DP-8 | 序列化 / 反序列化**对称**：round-trip 字段值不变 | P0 |
| DP-9 | nlohmann::json 序列化（与现有项目一致） | P0 |

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
├── CameraPath.h                ← 3D 样条路径
├── CameraRig.h                 ← 摄影机运动原语状态
├── CameraPreset.h              ← 摄影机预设定义
├── CameraShake.h               ← 摄影机抖动（噪声）
├── CameraLimiter.h             ← 限位器（AABB）
├── TimeRemap.h                 ← 时间重映射（速度曲线）
├── Marker.h                    ← 章节标记
├── Clip.h                      ← 视频剪辑
├── Track.h                     ← 时间轴轨道
├── Transition.h                ← 3 个基础转场
├── BezierCurve.h               ← 通用贝塞尔曲线
├── EditorLayout.h              ← 编辑器布局
├── EditorWorkspace.h           ← 工作区
├── KeyBinding.h                ← 快捷键绑定
├── RecordingProfile.h          ← 录制配置预设
├── EditorPreferences.h         ← 编辑器偏好
├── JsonCodec.h / .cpp          ← 通用 JSON 编 / 解码（模板特化）
└── Migration.h / .cpp          ← 版本迁移（v1 → v2 ...）
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

### 2.3 `EditorState`（`.playback` editor 节点）

```cpp
// EditorState.h
struct EditorState {
    int                        version{2};           // editor 节点版本
    std::vector<CameraTrackExt> tracks;              // 多摄影机轨道
    std::vector<Marker>         markers;             // 章节标记
    TimeRemap                   timeRemap;           // 时间重映射
    std::vector<Track>          videoTracks;         // 视频剪辑轨
    std::vector<Transition>     transitions;         // 转场（连接两个 Clip）
    std::vector<BezierCurve>    curves;              // 共享贝塞尔曲线
    int                         activeCameraTrackIdx{0};
    int                         activeVideoTrackIdx{0};
    float                       playheadTick{0};     // 编辑器光标位置
};
```

**`CameraTrackExt`**：在旧 [CameraTrack](../editor/camera-track.md) 基础上扩展：

```cpp
struct CameraTrackExt {
    std::string             name{"Main"};
    bool                    visible{true};
    CameraKind              kind{CameraKind::Keyframe};  // Keyframe / Path / Rig / Preset
    std::vector<CameraKeyframe> keys;                    // kind=Keyframe 用
    std::optional<CameraPath>   path;                    // kind=Path 用（3D 样条）
    std::optional<CameraRig>    rig;                     // kind=Rig 用（运动原语）
    std::optional<CameraPreset> preset;                  // kind=Preset 用
    std::vector<CameraShake>    shakes;                  // 任意 kind 都可叠加
    std::optional<CameraLimiter> limiter;                // 限位器
    std::string                 bindingEntityUuid;       // 绑定的实体（空 = 不绑）
    int                         bindingMode{0};          // 0=无 1=位置跟随 2=角度跟随 3=全跟随
    float                       bindingDamping{0.1f};    // 弹簧阻尼（0..1）
    std::optional<CameraPreset> override;                // 摄影机瞬时参数覆盖（FOV / roll / 偏移）
};
```

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

### 2.10 `TimeRemap`（时间重映射）

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
    // locateSegment + 插值（与 CameraTrack::locateSegment 一致）
}
```

**示例**：原 100tick 播放用 50 渲染 = 慢动作 ×0.5。

### 2.11 `Clip` / `Track` / `Transition`

```cpp
// Clip.h
struct Clip {
    std::string id;             // uuid
    std::string replayFile;     // .playback 路径
    int         inTick{};
    int         outTick{};
    int         trackTick{};    // 在 track 上的起始 tick
    int         activeCameraTrackIdx{0};
    float       speed{1.0f};    // 局部变速
    std::string name;
    Color4      color{Color4::Blue};
    bool        muted{false};
    bool        locked{false};
};

// Track.h
struct Track {
    std::string        id;
    std::string        name;
    TrackKind          kind{TrackKind::Video};  // Video / Camera / Marker
    std::vector<Clip>  clips;
    bool               visible{true};
    bool               locked{false};
    int                height{48};              // UI 像素高度
};

enum class TrackKind : uint8_t { Video = 0, Camera, Marker };
```

**Transition**（3 个基础）：

```cpp
enum class TransitionKind : uint8_t {
    Cut = 0,            // 硬切（duration=0）
    Fade,               // 淡入淡出
    CrossDissolve       // 交叉溶解
};

struct Transition {
    std::string    id;
    TransitionKind kind{TransitionKind::Cut};
    int            durationTicks{20};  // 0=Cut
    EasingType     easing{EasingType::EaseInOut};
    std::string    fromClipId;
    std::string    toClipId;
    Color4         fadeColor{Color4::Black};  // Fade 用
};
```

**应用算法**（以 CrossDissolve 为例）：

```cpp
float Transition::blendAlpha(int tickInTransition) const {
    float t = float(tickInTransition) / durationTicks;
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return easingValue(easing, t);
}

// RenderJob 应用：在 [transitionStart, transitionEnd] 区间同时渲染两个 Clip，alpha 混合
```

### 2.12 `BezierCurve`（通用曲线）

```cpp
struct BezierPoint {
    float t{0};             // x ∈ [0,1]
    float v{0};             // y
    Vec2  inTangent{0,0};   // 相对控制点偏移
    Vec2  outTangent{0,0};
};

struct BezierCurve {
    std::string            name;
    std::vector<BezierPoint> points;
    bool                   loop{false};

    // 任意 t (0..1) 采样 y
    float sample(float t) const;
    // 同 camera-track.md 的 CubicBezier Newton-Raphson 反函数
};
```

**用途**：可被 `CameraKeyframe.easing`（CubicBezier）、`RigSegment.easing`、`Transition.easing` 共用。

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
| 3 | `CameraPath` / `CameraRig` / `CameraPreset` / `CameraShake` / `CameraLimiter` 数据 + 序列化 | 单测：locate / sampleAt |
| 4 | `TimeRemap` / `Marker` | 单测：remap 边界 |
| 5 | `BezierCurve` | 单测：5 个采样点 |
| 6 | `Clip` / `Track` / `Transition` | 编译 |
| 7 | `CameraTrackExt`（含 EditorState） | 单测：JSON round-trip |
| 8 | `EditorLayout`（含 DockLayout 序列化） | 手动：保存 → 加载 → 布局恢复 |
| 9 | `EditorWorkspace` | 单测：可见面板列表 |
| 10 | `KeyBinding` / `KeyMap` | 单测：双向查找 |
| 11 | `RecordingProfile` | 单测：JSON round-trip |
| 12 | `EditorPreferences` | 单测：JSON round-trip |
| 13 | `MigrationChain` | 单测：v1 → v2 |
| 14 | `PlaybackMeta` 增 `editor` 字段 | 手动：旧 .playback 加载不报错 |
| 15 | 首次启动拷贝内置文件 | 手动：删 data/ → 启动 → 看到 default |

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
| DP-T1 | `CameraTrackExt` round-trip | 所有字段不变 |
| DP-T2 | 旧 `.playback`（无 editor 节点）加载 | `editor = nullopt`，不报错 |
| DP-T3 | v1 → v2 迁移 | 字段全映射 |
| DP-T4 | `fromJson` 缺字段 | 缺字段用默认 |
| DP-T5 | `BezierCurve.sample(0.5)` 线性 | y = (p0.v + p1.v) / 2 |
| DP-T6 | `TimeRemap.remap` 边界 | 端点返回对应 targetTick |
| DP-T7 | `validate` 命中不合法 fps | 含 E_FpsOutOfRange |
| DP-T8 | `EditorLayout` 序列化 | ImGui DockBuilder 可还原 |
| DP-T9 | `KeyMap` 双向查找 | ctrl+space → playback.togglePause |
| DP-T10 | `RecordingProfile` round-trip | 字段全一致 |

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

- **`refactor/camera-motion/*`**：用 `CameraTrackExt` / `CameraPath` / `CameraRig` / `CameraShake` / `CameraLimiter` / `CameraPreset`
- **`refactor/video-editing/*`**：用 `Clip` / `Track` / `Transition` / `BezierCurve`
- **`refactor/advanced-recording/*`**：用 `RecordingProfile`
- **`refactor/render-pipeline/*`**：用 `EditorState` / `TimeRemap` / `Marker` 渲染
- **`refactor/editor-architecture/*`**：用 `EditorLayout` / `EditorWorkspace` / `KeyMap` / `EditorPreferences`

### 调用谁（下游）

- **nlohmann::json**：序列化
- **stb_perlin.h**（新增）：抖动噪声
- **ImGui `DockBuilder`**：布局序列化

### 共享数据

- 无（纯数据结构）

### 事件订阅 / 发送

- 无

## 五、阅读顺序

1. 本文件
2. [02-camera-motion.md](02-camera-motion.md) —— 摄影机
3. [04-video-editing.md](04-video-editing.md) —— 剪辑
4. [03-advanced-recording.md](03-advanced-recording.md) —— 录制
5. [05-render-pipeline.md](05-render-pipeline.md) —— 渲染
6. [01-editor-architecture.md](01-editor-architecture.md) —— UI
