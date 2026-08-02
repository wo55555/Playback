# 04 · 多剪辑编辑（按 09 工作流 · 操作层）

> 入口：`src/playback/refactor/video-editing/`
> 角色：实现 [09-video-editing-workflow.md](09-video-editing-workflow.md) 所定义 **3 条一级轨道**（摄像机序列 / 世界Actor / 摄像机）下的**剪辑操作**：段 CRUD / 切 / trim / ripple / 绑定 / 关键帧 / Undo-Redo。**本文件是 09 的"操作层"**。
> 数据模型在 [09 §2.2-2.3](09-video-editing-workflow.md) 与 [06-data-persistence.md](06-data-persistence.md) 描述；本文件**只描述**命令、操作算法与 Undo/Redo 互逆性。
> **不**做内容浏览器、复杂转场、后期处理（与 09 一致）。

## 一、需求（Requirements）

> 工作流总需求见 [09 §1.1](09-video-editing-workflow.md)。本文件聚焦操作：

| ID | 需求 | 优先级 |
|---|---|---|
| VE-1 | **摄像机序列**：Split / Trim / Delete / BindCamera / SetSpeed 全套；操作可 Undo/Redo | P0 |
| VE-2 | **世界Actor**：Split / Trim / SetSpeed / RippleDelete 全套；操作可 Undo/Redo | P0 |
| VE-3 | **摄像机**：AddFree / CreateBinding(子Actor) / Delete(同时清空引用) / SetKind / Unbind | P0 |
| VE-4 | **关键帧**：Add / Move / Delete / SetEasing / SetValue（position/rotation/fov） | P0 |
| VE-5 | **子Actor**：SetDetails（按 category 的 agent 字段） | P1 |
| VE-6 | **Undo/Redo 栈**：≤ 100 步，所有命令 execute/undo 互逆 | P0 |
| VE-7 | 序列段与 WorldActor 段**首尾相接**（split 不留空隙，ripple 不留洞） | P0 |
| VE-8 | 删除 Camera 时，所有引用它的 sequence 段 `cameraId = ""`（导出兜底 cameras[0]） | P0 |
| VE-9 | Clip / Track 概念**整体下线**（旧 `TrackKind::Video/Camera/Marker` 与 `Clip/Track/Transition` 由 09 新模型替代） | P0 |

### 1.2 非功能性需求

- **操作响应**：单次编辑操作 < 16ms（1080p 视口无卡顿）
- **序列段数**：≤ 256（与 [09 §1.2](09-video-editing-workflow.md) 一致）
- **世界Actor 段数**：≤ 32
- **摄像机数**：≤ 16
- **Undo 栈**：≤ 100 步
- **持久化**：所有编辑操作写入 `EditorStateExt`，`record stop` 时落盘

### 1.3 与现有约束对齐

- 复用 [EditorContext](../editor/context/EditorContext.md) 的 mutex 模式
- 复用 [CommandStack](01-editor-architecture.md) 撤销 / 重做
- 不引入新的第三方库（[BezierCurve](06-data-persistence.md) 自写）
- 旧 `Track/Clip/Transition/TrackManager/TransitionEngine` 全部由新模型替代

## 二、架构（Architecture）

### 2.1 内部结构

```
refactor/video-editing/
├── SequenceSegment.{h,cpp}          ← 摄像机序列段（split/trim/bind/speed）
├── WorldActorSegment.{h,cpp}        ← 世界Actor 段（split/trim/speed/ripple）
├── CameraEntity.{h,cpp}             ← 摄像机（add free / create binding / keyframe）
├── SubActor.{h,cpp}                 ← 子Actor（category + agentDetails）
├── WorldActor.{h,cpp}               ← 容器：解析自 .playback
├── SequenceOps.{h,cpp}              ← 序列专用：findSegmentAt / mergeAfterDelete
├── CameraBindingOps.{h,cpp}         ← 绑定：从 SubActor 创建 Camera
├── commands/
│   ├── SequenceCommands.{h,cpp}     ← Split/Trim/Delete/Bind/SetSpeed
│   ├── WorldActorCommands.{h,cpp}   ← Split/Trim/SetSpeed/RippleDelete
│   ├── CameraCommands.{h,cpp}       ← AddFree/CreateBinding/Keyframe/SetKind/Unbind/Delete
│   ├── SubActorCommands.{h,cpp}     ← SetDetails
│   └── IEditCommand.h               ← Command 模式基类
├── SelectionModel.{h,cpp}           ← 选中模型（3 类别 + 段 + 关键帧）
├── BezierCurve.{h,cpp}              ← 通用曲线（被 Camera 关键帧 easing 共用）
└── BezierCurveEditor.{h,cpp}        ← 贝塞尔曲线 UI（在 Details 面板弹出）
```

### 2.2 `SequenceOps`（序列操作核心）

```cpp
namespace SequenceOps {

// O(log n)：找到含 tick 的段
const SequenceSegment* findSegmentAt(const std::vector<SequenceSegment>& segs, int tick);

// 校验：[0,totalTicks] 完全覆盖；段间无空隙
bool validateCoverage(const std::vector<SequenceSegment>& segs, int totalTicks);

// 在 atTick 切分含 atTick 的段；返回新段 id（若切不动 = atTick 在端点）
std::string splitAt(std::vector<SequenceSegment>& segs, int atTick);

// 段 delIdx 删除后，左右两段自动合并（覆盖连续）
void mergeAfterDelete(std::vector<SequenceSegment>& segs, size_t delIdx);

// 段 trim 头尾
void trimSegment(std::vector<SequenceSegment>& segs, const std::string& segId,
                 int newStart, int newEnd);

// 段改 speed
void setSegmentSpeed(SequenceSegment& seg, float speed);

// 段绑 Camera（清空 = ""，由导出兜底）
void bindCamera(SequenceSegment& seg, const std::string& cameraId);

}  // namespace
```

**算法**（`findSegmentAt`）：

```cpp
const SequenceSegment* findSegmentAt(const std::vector<SequenceSegment>& segs, int tick) {
    if (segs.empty()) return nullptr;
    auto it = std::upper_bound(segs.begin(), segs.end(), tick,
        [](int v, const SequenceSegment& s){ return v < s.startTick; });
    if (it == segs.begin()) return &segs.front();
    --it;
    return (tick < it->endTick) ? &*it : nullptr;
}
```

**算法**（`mergeAfterDelete`）：

```cpp
void mergeAfterDelete(std::vector<SequenceSegment>& segs, size_t delIdx) {
    if (delIdx > 0 && delIdx + 1 < segs.size()) {
        segs[delIdx - 1].endTick   = segs[delIdx + 1].endTick;
        segs[delIdx - 1].sourceTick = segs[delIdx + 1].sourceTick;
        segs.erase(segs.begin() + delIdx);
        segs.erase(segs.begin() + delIdx);  // 原 delIdx+1 现为 delIdx
    } else {
        segs.erase(segs.begin() + delIdx);
    }
}
```

### 2.3 `WorldActorOps`（世界Actor 操作核心）

```cpp
namespace WorldActorOps {

// 与 SequenceOps 类似的 split / trim / setSpeed / rippleDelete
std::string splitAt(std::vector<WorldActorSegment>& segs, int atTick);
void trimSegment(std::vector<WorldActorSegment>& segs, const std::string& segId,
                 int newStart, int newEnd);
void setSegmentSpeed(WorldActorSegment& seg, float speed);

// Ripple Delete：删除段且后续所有段前移 totalRemoved
void rippleDelete(std::vector<WorldActorSegment>& segs, const std::string& segId);

}  // namespace
```

### 2.4 `CameraBindingOps`（绑定生成）

```cpp
namespace CameraBindingOps {

// 由子Actor 一键生成一台 Camera：FollowEntity 预设 + 全跟随 + 阻尼
CameraEntity createBindingCamera(const SubActor& actor);

// 在 cameras 列表中找 id 匹配；空 = cameras[0] 兜底
const CameraEntity* resolveCamera(const std::vector<CameraEntity>& cameras,
                                 const std::string& cameraId);

// 删除 Camera 时，sequence 中所有引用它的段 cameraId = ""
void clearReferencesInSequence(std::vector<SequenceSegment>& segs,
                               const std::string& cameraId);

// 删除 Camera 时，subActor.boundCameraId 中所有引用它的项清空
void clearReferencesInSubActors(std::vector<SubActor>& subActors,
                                const std::string& cameraId);

}  // namespace
```

**`createBindingCamera` 实现**：

```cpp
CameraEntity CameraBindingOps::createBindingCamera(const SubActor& actor) {
    CameraEntity cam;
    cam.id = genUuid();
    cam.name = actor.name + " (bind)";
    cam.kind = CameraKind::Preset;
    cam.preset = CameraPreset{ /* PresetKind::FollowEntity, ... */ };
    cam.bindingEntityUuid = actor.id;
    cam.bindingMode = 3;  // 全跟随
    cam.bindingDamping = 0.15f;
    return cam;
}
```

### 2.5 `BezierCurve` 与 `BezierCurveEditor`

```cpp
class BezierCurveEditor {
public:
    void setCurve(const BezierCurve& curve);
    void setSampleRange(float tMin, float tMax);  // 默认 [0,1]

    // UI 交互
    void draw(ImDrawList* dl, Rect area);
    void handleInput(const ImGuiIO& io, Rect area);

    // 输出
    BezierCurve curve() const;
    float sampleAt(float t) const;
};
```

**UI 草图**（在 Details 面板内弹出）：

```
+------------------------------------------+
| y=1.0  ·                                ·|
|         /‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾     |
|        /                                 |
|       /     · ← 控制点                   |
|      /    /‾\                            |
|     /   /   \                            |
|    /  ·     ·                           |
|   / /        \                          |
|  ·/           \                         |
|  0.0          1.0                       |
+------------------------------------------+
```

> 完整绘制 / 采样算法与 [02 §2.4 CubicBezier](02-camera-motion.md) 一致；本文件不重复。

### 2.6 `SelectionModel`（3 类别 + 段 + 关键帧）

```cpp
enum class SelectionKind {
    None,
    Sequence,            // 选中"序列"本身（无段选）
    SequenceSegment,     // 选中某段
    WorldActor,          // 选中"世界Actor"本身
    WorldActorSegment,   // 选中某段
    Camera,              // 选中某台 Camera
    SubActor,            // 选中某子 Actor
    Keyframe,            // 关键帧
    Marker,
};

struct Selection {
    SelectionKind kind{SelectionKind::None};
    std::string   id;             // 上述对象的 id
    std::string   secondaryId;    // keyframe / marker 等附加 id
    int           anchorTick{0};
    int           focusTrackKind{0};  // 0=sequence 1=worldActor 2=camera
};

class SelectionModel {
public:
    void select(const std::string& id, SelectionKind kind, bool additive = false);
    void clear();
    bool isSelected(const std::string& id) const;
    Selection snapshot() const;
    void setAnchor(int tick);
};
```

**操作**：
- 单击：替换选中
- Ctrl+单击：加选
- Shift+单击：范围选
- 框选（Marquee）：拖拽框覆盖

### 2.7 命令模式（Undo/Redo）

```cpp
struct IEditCommand {
    virtual ~IEditCommand() = default;
    virtual void execute(EditorStateExt& s) = 0;
    virtual void undo(EditorStateExt& s) = 0;
    virtual std::string label() const = 0;
};

// 摄像机序列
class SplitSequenceAtPlayhead : public IEditCommand {
    int atTick;
    SequenceSegment newSeg;   // 保存 split 出的新段
public:
    void execute(EditorStateExt& s) override;  // SequenceOps::splitAt
    void undo(EditorStateExt& s) override;     // 删 newSeg.id
    std::string label() const override { return "Split Sequence"; }
};

class TrimSequenceSegment : public IEditCommand {
    std::string segId;
    int oldStart, oldEnd, newStart, newEnd;
public:
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override;
};

class BindSequenceToCamera : public IEditCommand {
    std::string segId, oldCam, newCam;
public:
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override;
};

class SetSequenceSegmentSpeed : public IEditCommand { /* 同 Trim 模式 */ };
class DeleteSequenceSegment : public IEditCommand { /* mergeAfterDelete undo 需保存 3 段快照 */ };

// 世界Actor
class SplitWorldActorAtPlayhead : public IEditCommand { /* 同上 */ };
class TrimWorldActorSegment : public IEditCommand { /* 同上 */ };
class SetWorldActorSegmentSpeed : public IEditCommand { /* 同上 */ };
class RippleDeleteWorldActorSegment : public IEditCommand {
    std::string segId;
    std::vector<WorldActorSegment> movedSnapshot;  // undo 还原
public:
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override;
};

// 摄像机
class AddFreeCamera : public IEditCommand {
    CameraEntity cam;
    int insertIdx;
public:
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override;
};

class CreateBindingCamera : public IEditCommand {
    CameraEntity cam;
    std::string subActorId;  // undo 还原 boundCameraId
    std::string oldBound;
public:
    void execute(EditorStateExt& s) override {
        // 1) s.cameras.push(cam)
        // 2) s.worldActor.subActors[id].boundCameraId = cam.id
    }
    void undo(EditorStateExt& s) override {
        // 1) s.cameras remove by cam.id
        // 2) restore oldBound
    }
    std::string label() const override { return "Create Camera Binding"; }
};

class DeleteCamera : public IEditCommand {
    CameraEntity cam;
    std::vector<std::string> clearedSegIds;        // undo 还原
    std::map<std::string, std::string> clearedSubActor;  // 同上
    int insertIdx;
public:
    void execute(EditorStateExt& s) override {
        // 1) Snapshot
        // 2) cameras erase
        // 3) CameraBindingOps::clearReferencesInSequence
        // 4) CameraBindingOps::clearReferencesInSubActors
    }
    void undo(EditorStateExt& s) override;
    std::string label() const override;
};

// 关键帧
class AddKeyframe : public IEditCommand { /* ... */ };
class MoveKeyframe : public IEditCommand { /* ... */ };
class DeleteKeyframe : public IEditCommand { /* ... */ };
class SetKeyframeEasing : public IEditCommand { /* ... */ };
class SetKeyframeValue : public IEditCommand { /* ... */ };

// 子Actor
class SetSubActorDetails : public IEditCommand { /* ... */ };
```

**`CommandStack`**（[01 §2.13](01-editor-architecture.md) 中详述）：

```cpp
class CommandStack {
public:
    void push(std::unique_ptr<IEditCommand> cmd);
    bool undo();
    bool redo();
    void clear();
    std::vector<std::string> undoLabels() const;
    std::vector<std::string> redoLabels() const;
private:
    std::vector<std::unique_ptr<IEditCommand>> mUndo;
    std::vector<std::unique_ptr<IEditCommand>> mRedo;
};
```

**绑定 UI**：
- `Ctrl+Z` → `CommandStack::undo()`
- `Ctrl+Shift+Z` / `Ctrl+Y` → `CommandStack::redo()`

### 2.8 时间轴渲染（参考，详细在 [08](08-sequencer-timeline-ui.md)）

```
+------------------------------------------------------------------+
|  ◀ ▶ ⏸  [00:01:23.456]  ↺ ↻  📍 [Marker]                       |
+------------------------------------------------------------------+
|  0:00      0:30      1:00      1:30      2:00                    |
|  |---------|---------|---------|---------|                        |
|  S:   [== Segment A (Cam0) ==][= Seg B (Cam2) =][== Seg C (auto) ==]
|  W:        [== WorldActor A ==][== WorldActor B ==]               |
|  C0:  ════●══════●══════●══════●══════●═  Main                    |
|  C1:       ════●══════●══════●══════       Player1 (bind)         |
+------------------------------------------------------------------+
```

**视觉**：
- **摄像机序列**（S）：横向矩形，按"绑定 Camera"着色；未绑定 = 灰底斜线
- **世界Actor**（W）：横向矩形，橙色（回放原色）
- **摄像机**（Cn）：细线 + 关键帧点 ◆
- **拖拽**：段头/尾 → Trim；段体 → Move（**段间必须接续，不允许重叠**）
- **分割线**：playhead 垂直黄线
- **子Actor 不画在画布**；在 Details 面板用树展示

### 2.9 与 [02](02-camera-motion.md) CameraSystem 的集成

**核心变化**：`Clip.activeCameraTrackIdx` 模式**完全移除**。新模式下：
- `CameraSystem::sampleAt(camera, tick, session)` 直接接受 `CameraEntity`（取代旧 `trackIdx`）。
- 序列段调用 `resolveCamera(seq.cameraId, e.cameras)` 拿到 `CameraEntity*`。
- 导出 / 预览按 [09 §2.7-2.8](09-video-editing-workflow.md) 的伪代码。

```cpp
// 新：直接传 CameraEntity
CameraSample CameraSystem::sampleAt(const CameraEntity& cam, int tick,
                                    const ReplaySession& session) const {
    // 1) 基础：按 cam.kind 采样
    CameraSample s = dispatchByKind(cam, tick, session);
    // 2) 绑定 + 阻尼
    if (!cam.bindingEntityUuid.empty()) s = applyBinding(s, cam, tick, session);
    // 3) Shake / Limiter（与 02 一致）
    return s;
}
```

> 旧 `CameraSystem::sampleAt(tick, session)` 仍保留为"按 `activeCameraIndex` 选一台 Camera 采样"的快捷入口，用于"无序列选中 / 单 Camera 预览"。

## 三、执行（Execution）

### 3.1 任务拆分

| # | 文件 | 内容 | 验证 |
|---|---|---|---|
| 1 | `SequenceSegment.{h,cpp}` | 序列段模型 + 序列化 | 编译 |
| 2 | `WorldActorSegment.{h,cpp}` | 世界Actor 段模型 + 序列化 | 编译 |
| 3 | `CameraEntity.{h,cpp}` | 摄像机实体 | 编译 |
| 4 | `SubActor.{h,cpp}` | 子Actor + 类别枚举 | 编译 |
| 5 | `WorldActor.{h,cpp}` | 容器：解析自 .playback | 编译 |
| 6 | `SequenceOps.{h,cpp}` | findSegmentAt / mergeAfterDelete / splitAt | 单测：覆盖 / 拆分 / 合并 |
| 7 | `WorldActorOps.{h,cpp}` | splitAt / trimSegment / rippleDelete | 单测：ripple 前移 |
| 8 | `CameraBindingOps.{h,cpp}` | createBinding / resolve / clearReferences | 单测：引用清空 |
| 9 | `commands/SequenceCommands` | Split/Trim/Delete/Bind/SetSpeed | 单测：execute/undo 互逆 |
| 10 | `commands/WorldActorCommands` | Split/Trim/SetSpeed/RippleDelete | 单测：execute/undo 互逆 |
| 11 | `commands/CameraCommands` | AddFree/CreateBinding/Keyframe/SetKind/Unbind/Delete | 单测：execute/undo 互逆 |
| 12 | `commands/SubActorCommands` | SetDetails | 单测：execute/undo 互逆 |
| 13 | `BezierCurve.{h,cpp}` + `BezierCurveEditor.{h,cpp}` | 曲线 + UI | 手动：手画曲线 |
| 14 | `SelectionModel.{h,cpp}` | 8 种 SelectionKind | 单测：替换/加选/范围选 |
| 15 | 集成到 `TimelinePanel` | 渲染 3 一级轨道 + 段 + 关键帧 | 手动：UI 正确 |
| 16 | 集成到 `DetailsPanel` | 8 个上下文 | 手动：每个上下文 |
| 17 | 集成到 `CommandStack` | push/undo/redo | 单测：100 步 |
| 18 | 集成到 `RenderJob` / `RealtimePreview` | 沿序列导出 / 预览 | 手动：导出样片 |
| 19 | 移除旧 `TrackManager` / `TransitionEngine` | 旧 API 下线 | 编译 |

### 3.2 关键算法

**段 sort by tick**（insert 后必做）：

```cpp
void sortSegmentsByTick(std::vector<SequenceSegment>& segs) {
    std::sort(segs.begin(), segs.end(),
              [](const SequenceSegment& a, const SequenceSegment& b){
                  return a.startTick < b.startTick;
              });
}
```

**段 split（不变量保持）**：

```cpp
std::string SequenceOps::splitAt(std::vector<SequenceSegment>& segs, int atTick) {
    auto* seg = findSegmentAt(segs, atTick);
    if (!seg) return {};
    if (atTick <= seg->startTick || atTick >= seg->endTick) return {};

    SequenceSegment right = *seg;
    right.id = genUuid();
    right.startTick   = atTick;
    right.sourceTick += (atTick - seg->startTick);

    seg->endTick = atTick;

    segs.push_back(right);
    sortSegmentsByTick(segs);
    return right.id;
}
```

**导出伪代码**（[09 §2.7](09-video-editing-workflow.md) 详）：

```cpp
void RenderJob::runExport(EditorStateExt& e) {
    for (int frame = 0; frame < totalFrames; ++frame) {
        int timelineTick = frame * ticksPerFrame;

        const SequenceSegment* seg = SequenceOps::findSegmentAt(e.sequence, timelineTick);
        if (!seg) continue;

        const CameraEntity* cam = CameraBindingOps::resolveCamera(e.cameras, seg->cameraId);
        if (!cam) continue;

        int localTick = timelineTick - seg->startTick;
        int sourceTick = seg->sourceTick + (int)(localTick * seg->speed);

        replaySession.requestSeek(sourceTick);
        replaySession.tick();

        CameraSample s = CameraSystem::getInstance().sampleAt(*cam, sourceTick, replaySession);
        CameraSystem::applyToMCBE(s);
        captureFrame();
    }
}
```

### 3.3 关键不变量

1. **段首尾相接**：`validateCoverage(segs, totalTicks) == true` 永真。
2. **段不重叠**：split 后两段共端点（`prev.endTick == next.startTick`）。
3. **Ripple 不留洞**：被删段 + 后续段前移 = 仍覆盖 `[0, totalTicks]`。
4. **Camera 引用一致性**：`segment.cameraId != ""` 时，cameras 中存在该 id；否则在导出兜底 `cameras[0]`。
5. **绑定唯一**：单个 SubActor 同时最多被 1 台 Camera 绑定。
6. **Command execute/undo 互逆**：undo 后状态 == 执行前状态（property test）。
7. **Selection 唯一**：UI 上只显示一个"主选中"（其余浅色）。
8. **删除 Camera 清空引用**：所有 sequence 段 + 所有 subActor 的 boundCameraId 中含该 id 的项置空（不抛错）。

### 3.4 测试用例

| ID | 用例 | 期望 |
|---|---|---|
| VE-T1 | 打开 .playback → 序列默认 1 段 [0, totalTicks] | cameras 空，段未绑 |
| VE-T2 | split sequence at tick=1000 | 变 2 段 [0,1000) + [1000,totalTicks) |
| VE-T3 | trim worldActor 段 in +20 | startTick += 20 |
| VE-T4 | rippleDelete worldActor 中段 | 后续段前移 = 仍覆盖 totalTicks |
| VE-T5 | 子Actor 树展开 Players | Details 面板出现按名字排序的玩家列表 |
| VE-T6 | 玩家右键"创建摄像机绑定" | cameras 多 1 台；subActor.boundCameraId 填 |
| VE-T7 | 序列段绑到新建 Camera | segment.cameraId 更新；导出用该 Camera |
| VE-T8 | 删除 Camera | cameras 少 1；引用它的段变 cameraId=""；引用它的 subActor 清空 boundCameraId |
| VE-T9 | 关键帧 CRUD | 关键帧点增删 + UI 重绘 |
| VE-T10 | undo CreateBindingCamera | cameras 恢复；subActor.boundCameraId 清空 |
| VE-T11 | 导出 = 沿序列渲染 | 导出视频按时序切镜头 |
| VE-T12 | 100 步 undo + redo | 栈不丢 |
| VE-T13 | 旧 .playback（无 sequence/worldActor/cameras 字段）加载 | 重建：序列 1 段、世界Actor 1 段、cameras 空 |
| VE-T14 | DeleteSequenceSegment（中段） | 左右两段合并 |
| VE-T15 | SetKeyframeEasing | 关键帧 easing 字段更新；不破坏其他帧 |

### 3.5 风险与回退

| 风险 | 缓解 |
|---|---|
| 子Actor 数量大（>500）树渲染卡 | 默认折叠；首次展开只渲染前 100 |
| 绑定 Camera 解绑后旧段无 Camera | 兜底 cameras[0] + UI 警告 |
| 旧 videoTracks 字段被移除，存档不可读 | JSON 缺字段重建；新存档不写旧字段 |
| 关键帧绑定 Camera id 在重命名后失效 | cameraId 用 uuid，不依赖 name |
| 沿序列导出时 WorldActor 段与序列段不对齐 | 导出前校验：每个 sequence 段必须有对应 worldActor 段覆盖其 [startTick,endTick)，否则报 ErrorDialog |
| Ripple 后段重叠 | validateCoverage 在 execute 末尾必调；不通过则回滚 |
| 撤销栈内存膨胀 | maxUndoSteps 100 截断 |
| 旧 Clip.activeCameraTrackIdx 残留引用 | 编译期删除；旧文档标注 DEPRECATED |

## 四、模块关系

### 被谁调用（上游）

- **`refactor/editor/panels/TimelinePanel`**：渲染 3 一级轨道 + 段 + 关键帧
- **`refactor/editor/panels/DetailsPanel`**：上下文敏感字段编辑
- **`refactor/editor/panels/SubActorTree`**（新）：子Actor 树
- **`refactor/editor/EditorBridge`**：同步 sequence / worldActor / cameras
- **`refactor/render-pipeline/RealtimePreview`**：序列驱动预览
- **`refactor/render-pipeline/RenderJob`**：沿序列导出
- **`refactor/editor/CommandStack`**：包装所有命令

### 调用谁（下游）

- **[01](01-editor-architecture.md) EditorCore**：EditorStateExt / CommandStack
- **[02](02-camera-motion.md) CameraSystem**：sampleAt + applyToMCBE（不再用 `activeCameraTrackIdx`）
- **[05](05-render-pipeline.md) RenderJob / RealtimePreview**：读 sequence 与 worldActor
- **[06](06-data-persistence.md) PlaybackMeta.editor**：序列化 sequence / worldActor / cameras / subActors
- **旧 [EditorContext](../editor/context/EditorContext.md)**：通过 `EditorBridge` 通信

### 共享数据

- `EditorStateExt.sequence`：UI ↔ 渲染
- `EditorStateExt.worldActor`：UI ↔ 渲染
- `EditorStateExt.cameras`：UI ↔ 渲染
- `EditorStateExt.activeCameraIndex`：gizmo 高亮
- `SelectionModel`：UI 选中

### 事件订阅 / 发送

- `CommandStack.onPushed/onUndo/onRedo` → StatusPanel
- `SequenceOps.onSegmentsChanged` → TimelinePanel
- `CameraBindingOps.onBindingCreated` → ViewportPanel（重置 gizmo）

## 五、阅读顺序

1. 本文件（操作层）
2. [09](09-video-editing-workflow.md) — 工作流总览
3. [01](01-editor-architecture.md) — 编辑器骨架
4. [08](08-sequencer-timeline-ui.md) — Sequencer 四区 UI
5. [02](02-camera-motion.md) — CameraSystem 算法
6. [05](05-render-pipeline.md) — 渲染消费
7. [06](06-data-persistence.md) — 数据模型
