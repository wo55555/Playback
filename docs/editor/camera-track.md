# editor/camera-track — 摄像机轨道（关键帧 + 插值 + ImGui 编辑）

> 入口：`src/playback/editor/camera-track/`
> 角色：扩展现有回放编辑器，新增 **摄影机轨道（Camera Track）** 作为可被 `RenderJob` 采样的关键帧序列，并提供 ImGui 编辑面板。
> 与现有 `editor/` 的关系：复用 `EditorContext` 的状态/动作协议，新增 `CameraTrackState` / `CameraTrackAction`。

## 一、需求（Requirements）

### 1.1 功能性需求

| ID | 需求 | 优先级 |
|---|---|---|
| CT-1 | 用户可在回放编辑器中**创建 / 删除 / 选中**多条摄影机轨道（默认 1 条主轨） | P0 |
| CT-2 | 每条轨道由**关键帧**组成，**至少**包含：`tick`、`position(x,y,z)`、`rotation(yaw,pitch)`、`fov`、`easing` | P0 |
| CT-3 | 关键帧支持 **insert / delete / drag / snap-to-tick** 四种编辑操作 | P0 |
| CT-4 | 提供 **5 种 easing**：Linear / EaseIn / EaseOut / EaseInOut / CubicBezier（控制点） | P0 |
| CT-5 | `RenderJob` 可在给定 tick 上**采样**轨道当前状态（位置 / 旋转 / FOV），采样必须确定性、可重放 | P0 |
| CT-6 | 轨道数据与 `.playback` 文件**绑定**（同一录制 → 同一轨道，导出时一致） | P0 |
| CT-7 | 支持 **轨道预设**：第一人称 / 第三人称跟随 / 自由环绕 / 固定机位 | P1 |
| CT-8 | 关键帧编辑操作可 **undo / redo**（基于 `EditorContext` 扩展的 `CameraTrackAction`） | P1 |
| CT-9 | 轨道数据**持久化**到回放元数据 `.playback` 内部，与回放 ZIP 一同打包 | P1 |

### 1.2 非功能性需求

- **性能**：单轨道 ≤ 256 关键帧时，采样延迟 < 0.1ms（无锁查询）。
- **可重放性**：相同 `tick` 多次采样必须返回相同结果（浮点 `epsilon ≤ 1e-5`）。
- **线程安全**：UI 线程写、RenderJob 主线程读；通过 `EditorContext` 扩展的 snapshot 机制同步。
- **数据体积**：256 关键帧序列化后 ≤ 32KB。

### 1.3 与现有约束的对齐

- 复用 `EditorContext`（[context/EditorContext.h](file:///d:/raplay/Playback/src/playback/editor/context/EditorContext.h)）的 mutex 模式，新增 `CameraTrackSnapshot` 字段。
- 复用 `EditorAction` 模式，新增 `CameraTrackActionType` 枚举。
- 复用 ImGui + D3D12 渲染链路（[editor/renderer/ImGuiRenderer.h](file:///d:/raplay/Playback/src/playback/editor/renderer/ImGuiRenderer.h)）。
- 关键帧 tick 范围 = `[0, ReplaySession::getTotalTicks()]`。

## 二、架构（Architecture）

### 2.1 内部结构

```
editor/camera-track/
├── CameraTrack.h / .cpp           ← 轨道 + 关键帧数据模型 + 序列化
├── CameraSampler.h / .cpp         ← 在 tick 上确定性采样（RenderJob 调用）
├── CameraTrackContext.h / .cpp    ← 线程安全中转（接 EditorContext）
├── EditorCameraTrackExtension.h   ← EditorContext 扩展点
└── (UI 入口在 editor/ui/panels/CameraTrackPanel.*)
```

### 2.2 数据模型

```cpp
// CameraTrack.h

enum class EasingType : uint8_t {
    Linear = 0,
    EaseIn, EaseOut, EaseInOut,
    CubicBezier  // 额外带 2 个控制点
};

struct CameraKeyframe {
    int          tick{};        // 单位：client tick
    Vec3         position{};    // 摄影机世界坐标
    Vec2         rotation{};    // (yaw, pitch)，弧度
    float        fov{90.0f};    // 垂直 FOV，度
    EasingType   easing{EasingType::Linear};
    Vec2         bezierCtrl{0.42f, 0.58f};  // 仅 CubicBezier 使用
};

struct CameraTrack {
    std::string                name{"Main"};
    std::vector<CameraKeyframe> keys{};       // 升序，tick 单调
    bool                       visible{true};

    // O(log n) 在 tick 上定位段 [keys[i], keys[i+1]]
    std::pair<size_t, size_t> locateSegment(int tick) const;
};

struct CameraTrackSnapshot {
    std::vector<CameraTrack> tracks;          // 全部轨道
    int                     activeTrackIdx{0};
    int                     selectedKeyIdx{-1};
};
```

**关键设计**：

- `Vec3` / `Vec2` 复用 MCBE 内部 `Vec3` / `Vec2`（避免浮点二次转换）。
- `tick` 强制升序：`insert` 时二分定位；删除时 `erase-remove` 不重排（O(n) 可接受）。
- `CubicBezier` 的两个控制点用 `Vec2` 表达，x 轴 ∈ [0,1]、y 轴无限制（CSS 风格）。

### 2.3 采样算法

```cpp
// CameraSampler.h
struct CameraSample {
    Vec3  position;
    Vec2  rotation;
    float fov;
    bool  valid;  // false = 轨道为空 / tick 越界
};

CameraSample sampleAt(const CameraTrack& track, int tick);
```

**算法**：

1. 轨道空 → `{valid=false}`。
2. tick ≤ keys[0].tick → 直接返回 keys[0] 状态。
3. tick ≥ keys.back().tick → 返回 keys.back() 状态。
4. 二分定位段 `[A, B]`，计算 `t = (tick - A.tick) / float(B.tick - A.tick)`。
5. 对 `position` / `rotation` / `fov` 三个分量独立按 `easing` 插值：

| Easing | 公式 |
|---|---|
| Linear | `lerp(A, B, t)` |
| EaseIn | `lerp(A, B, t*t)` |
| EaseOut | `lerp(A, B, 1 - (1-t)*(1-t))` |
| EaseInOut | `lerp(A, B, t < 0.5 ? 2*t*t : 1 - pow(-2*t+2, 2)/2)` |
| CubicBezier | De Casteljau 在控制点 `(0,0)→(c1x,c1y)→(c2x,c2y)→(1,1)` 上求 x→y 反函数后取 y |

> **确定性保证**：所有插值都用 `float` 精确算；不引入 `std::chrono`、随机数。

### 2.4 与 EditorContext 的集成

```cpp
// EditorCameraTrackExtension.h
class EditorContextCameraExt {
public:
    // 由 UI 线程（renderer）调
    void submitCameraAction(CameraTrackAction action);
    CameraTrackSnapshot snapshotCamera() const;

    // 由 controller 线程（主线程）调
    std::vector<CameraTrackAction> takeCameraActions();
    void publishCameraSnapshot(CameraTrackSnapshot snap);

    // 在 EditorContext::reset() 时也清空
    void resetCamera();

private:
    mutable std::mutex mMtx;
    CameraTrackSnapshot mSnap;
    std::vector<CameraTrackAction> mActions;
};
```

**集成方式**（修改现有 `EditorContext`）：

- 把 `EditorContextCameraExt` 作为 `EditorContext` 的成员（**组合**，不继承）。
- `EditorContext::reset()` 内调 `mCameraExt.resetCamera()`。
- `EditorContext::snapshot()` 同时返回 `EditorState` + `CameraTrackSnapshot`（合并结构体或 `tie`）。
- `EditorContext::takeActions()` 合并返回 `EditorAction` + `CameraTrackAction`（`std::variant` 列表）。

### 2.5 持久化格式

`.playback` ZIP 内 `metadata.json` 增加 `cameraTracks` 字段：

```json
{
  "cameraTracks": {
    "version": 1,
    "tracks": [
      {
        "name": "Main",
        "visible": true,
        "keys": [
          { "tick": 0,    "px": 0,  "py": 80, "pz": 0,  "ry": 0,   "rp": 0,   "fov": 90, "easing": 0 },
          { "tick": 1200, "px": 10, "py": 80, "pz": 10, "ry": 1.5, "rp": -0.2, "fov": 70, "easing": 2, "cx": 0.42, "cy": 0.58 }
        ]
      }
    ]
  }
}
```

`PlaybackMeta`（[functions/record/Recorder.h:38](file:///d:/raplay/Playback/src/playback/functions/record/Recorder.h#L38)）新增 `nlohmann::json cameraTracks` 字段；`toJson` / `fromJson` 自动序列化。

### 2.6 渲染管线注入（如何让 MCBE 摄影机跑在轨道上）

**关键问题**：MCBE 的玩家相机位置 / 旋转 / FOV 由 `CameraManager` / `ClientPlayer` 内部管理，**不能**直接被外部代码改写而不触发服务端校验。

**方案**：在 `RenderJob` 启动时把 `ReplaySession` 切换到 **Editor Render Mode**（详见 [render-job.md §2.3](file:///d:/raplay/Playback/docs/functions/render/render-job.md)）—— 该模式下：

- `ReplaySession` 接受外部的"摄影机覆盖"（`setExternalCameraOverride(pos, rot, fov)`）。
- 在 `_subTick` 末尾（`ClientTickHooks`）的 `RenderJob::onTickEnd` 钩子里写入 `CameraManager::mCurrentCameraPosition` / `mCurrentCameraRotation` / `mFov`。
- MCBE 渲染线程下一帧直接读这些值，等价于"摄影机在轨道上"。

> **隔离**：Editor Render Mode 强制要求当前世界 = `__playback_replay_world__`，否则拒绝。避免污染在线服务器。

### 2.7 UI（ImGui）布局

```
+--------------------------------------------------+
| [回放编辑器 主窗口]                               |
| ┌─ Camera Track Panel (新增) ──────────────────┐ |
| │ Track: [Main ▼]  [+ Add] [Del] [Preset ▼]   │ |
| │ ┌─ Time Line (与 TimelinePanel 共享) ────┐  │ |
| │ │ · · ◆·······◆··········◆··········◆·   │  │ |
| │ │ 0    600   1200   1800   2400   3000   │  │ |
| │ └────────────────────────────────────────┘  │ |
| │ Selected Key:  tick=1200  pos=(10,80,10)    │ |
| │                rot=(1.50,-0.20)  fov=70°    │ |
| │                easing: [EaseInOut ▼]         │ |
| │ [Insert] [Delete] [Snap to tick]            │ |
| └─────────────────────────────────────────────┘ |
+--------------------------------------------------+
```

`CameraTrackPanel` 由 `ReplayView` 在 `editor/ui/ReplayView.cpp` 内嵌入到右侧 dock；通过 `EditorContextCameraExt` 读写。

## 三、执行（Execution）

### 3.1 任务拆分（实现顺序）

| 步骤 | 文件 | 验证 |
|---|---|---|
| 1 | `editor/camera-track/CameraTrack.{h,cpp}` | 单元测试：locateSegment / sampleAt |
| 2 | `editor/camera-track/CameraSampler.{h,cpp}` | 单元测试：5 种 easing 数值正确性 |
| 3 | `editor/camera-track/EditorCameraTrackExtension.h` + 改 `EditorContext` | 编译通过；原行为不退化 |
| 4 | `functions/record/Recorder.h` 增 `cameraTracks` JSON 字段 | 录制 → 导出 → 重读 元数据一致 |
| 5 | `editor/ui/panels/CameraTrackPanel.{h,cpp}` | 手动：增删关键帧；undo/redo |
| 6 | `ReplaySession::setExternalCameraOverride` | 主线程读写 MCBE `CameraManager` 不崩 |
| 7 | 与 `RenderJob` 集成（详见 render-job.md） | 导出测试样片分辨率/位置/FOV 正确 |

### 3.2 关键算法实现要点

**`locateSegment`**：二分 + `std::upper_bound` 自定义比较器：

```cpp
std::pair<size_t, size_t> CameraTrack::locateSegment(int tick) const {
    if (keys.empty()) return {0, 0};
    if (tick <= keys.front().tick) return {0, 0};
    if (tick >= keys.back().tick)  return {keys.size()-1, keys.size()-1};

    auto it = std::upper_bound(keys.begin(), keys.end(), tick,
        [](int value, const CameraKeyframe& kf){ return value < kf.tick; });
    size_t hi = std::distance(keys.begin(), it);
    size_t lo = hi - 1;
    return {lo, hi};
}
```

**CubicBezier 反函数**：用 Newton-Raphson（5 次迭代收敛到 1e-6）：

```cpp
float cubicBezierY(float t, Vec2 c1, Vec2 c2) {
    // 1) Newton: 求 x(t) = u → t
    auto xOfT = [&](float t){ /* 3 次贝塞尔 x 公式 */ };
    float u = t;
    for (int i = 0; i < 5; ++i) {
        float x = xOfT(u) - t;
        if (std::abs(x) < 1e-6f) break;
        u -= x / xDerivative(u);
    }
    // 2) y(u) 公式直接算
    return yOfT(u);
}
```

### 3.3 关键不变量

1. **轨道 tick 单调递增**：`insertKey` 后必须 `std::sort`；破坏不变量的操作回滚并 warn。
2. **采样是纯函数**：`sampleAt` 不读全局状态，只读 `track` 引用；线程安全。
3. **关键帧不引用外部资源**：序列化/反序列化不依赖 MCBE 内部符号，避免回放文件跨版本不兼容。
4. **轨道与 `.playback` 一一对应**：ZIP 内 `metadata.json.cameraTracks` 缺失时回退到"自由环绕"默认轨道（不阻塞回放）。

### 3.4 测试用例

| ID | 用例 | 期望 |
|---|---|---|
| CT-T1 | 空轨道 + tick=100 | `valid=false` |
| CT-T2 | 单关键帧 (tick=0) + tick=100 | 返回 keys[0]，valid=true |
| CT-T3 | 两关键帧 Linear + tick 中点 | 严格 `lerp` 中点 |
| CT-T4 | CubicBezier 端点 | x=0 → y=0；x=1 → y=1 |
| CT-T5 | 256 关键帧 + tick=mid | locateSegment O(log n)，sampleAt 延迟 < 0.1ms |
| CT-T6 | undo/redo 增删关键帧 | 数据完全一致 |
| CT-T7 | `.playback` 序列化 round-trip | JSON 字段值不变 |

### 3.5 风险与回退

| 风险 | 缓解 |
|---|---|
| MCBE 摄影机覆盖触发服务端校验 | 仅在 `__playback_replay_world__`（本地隔离世界）启用 |
| ImGui 撤销栈膨胀 | 限制 max=100；超出丢最早的 |
| 关键帧 tick 重复 | `insertKey` 检测到重复 tick → 替换而非插入 |

## 四、模块关系

### 被谁调用（上游）

- **`editor/ui/panels/CameraTrackPanel`**：读写轨道（UI 入口）。
- **`editor/controller/EditorController`**：每 tick 调 `publishCameraSnapshot` + `takeCameraActions`。
- **`functions/render/RenderJob`**：每帧调 `sampleAt` 拿当前摄影机状态。
- **`functions/render/FrameSource`**：把采样结果注入 MCBE `CameraManager`。

### 调用谁（下游）

- **`ReplaySession::setExternalCameraOverride`**（扩展点，新增）。
- **`PlaybackMeta::cameraTracks`** JSON 字段（[functions/record/Recorder.h](file:///d:/raplay/Playback/src/playback/functions/record/Recorder.h)）。
- **`EditorContext` / `EditorContextCameraExt`**：状态/动作中转。

### 共享数据

- `EditorContext::mCameraExt`：所有 UI 写入、controller 读出、RenderJob 读 snapshot。

### 事件订阅 / 发送

- 不订阅新事件；通过 `ClientTickHooks` 间接触发 controller 同步。

## 五、阅读顺序

1. 本文件
2. [editor/export-config.md](file:///d:/raplay/Playback/docs/editor/export-config.md)：导出配置数据模型
3. [functions/render/render-job.md](file:///d:/raplay/Playback/docs/functions/render/render-job.md)：RenderJob 如何消费轨道
