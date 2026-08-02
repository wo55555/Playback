# 04 · 多剪辑编辑

> 入口：`src/playback/refactor/video-editing/`
> 角色：在 [TimelinePanel](01-editor-architecture.md) 内提供 **多 Clip / 多 Track / 3 个基础转场 / Bezier 曲线** 的剪辑能力。**不**做内容浏览器、复杂转场、后期处理。
> 数据模型见 [06-data-persistence.md](06-data-persistence.md)；本文件描述**算法与编辑操作**。

## 一、需求（Requirements）

### 1.1 功能性需求

| ID | 需求 | 优先级 |
|---|---|---|
| VE-1 | 时间轴支持**多条**视频轨（Video）+ 摄影机轨（Camera）+ 标记轨（Marker） | P0 |
| VE-2 | 每条 Track 可放多个 Clip（同一 `.playback` 的不同区段） | P0 |
| VE-3 | Clip 操作：Cut（剪切到剪贴板）/ Split（按 playhead 切）/ Trim In / Trim Out / Ripple Delete | P0 |
| VE-4 | Clip 可独立设置 speed（局部变速，不改原回放） | P1 |
| VE-5 | Clip 可独立设置 activeCameraTrackIdx（每段用不同摄影机） | P0 |
| VE-6 | 3 个基础转场：**Cut**（duration=0）/ **Fade**（淡入淡出到指定色）/ **CrossDissolve**（交叉溶解） | P0 |
| VE-7 | 转场放在两个相邻 Clip 之间，UI 显示为半透明矩形 | P0 |
| VE-8 | **Bezier 曲线**：通用曲线编辑器（替代纯 easing），可被摄影机 / Rig / Transition 共用 | P0 |
| VE-9 | 撤销 / 重做：所有 Clip / Track / Transition 操作可 Undo / Redo | P0 |
| VE-10 | Clip / Track 可 Lock（防误编辑）和 Mute（不渲染） | P0 |

### 1.2 非功能性需求

- **操作响应**：单次编辑操作 < 16ms（1080p 视口无卡顿）
- **轨道数**：单项目支持 ≤ 32 视频轨 + 16 摄影机轨 + 1 标记轨
- **Clip 数**：单轨道 ≤ 256 个 Clip
- **Undo 栈**：≤ 100 步（与 [EditorPreferences.maxUndoSteps](06-data-persistence.md) 一致）
- **持久化**：所有编辑操作写入 `EditorState`，`record stop` 时落盘

### 1.3 与现有约束对齐

- 复用 [EditorContext](../editor/context/EditorContext.md) 的 mutex 模式
- 复用 [CommandStack](01-editor-architecture.md) 撤销 / 重做（待写）
- 不引入新的第三方库（[BezierCurve](06-data-persistence.md) 自写）

## 二、架构（Architecture）

### 2.1 内部结构

```
refactor/video-editing/
├── ClipEditor.{h,cpp}               ← Clip 增删改 + Cut/Split/Trim
├── TrackManager.{h,cpp}             ← 多轨管理
├── TransitionEngine.{h,cpp}         ← 转场混合
├── BezierCurveEditor.{h,cpp}        ← 贝塞尔曲线 UI + 采样
├── Clipboard.{h,cpp}                ← 内部剪贴板
├── SelectionModel.{h,cpp}           ← 选中模型
└── EditingCommands.h                ← Command 模式（Undo / Redo）
```

### 2.2 `TrackManager`（多轨）

```cpp
class TrackManager {
public:
    static TrackManager& getInstance();

    void setEditorState(EditorState state);
    EditorState snapshot() const;

    // 增删
    std::string addTrack(TrackKind kind, std::string name);
    void removeTrack(const std::string& id);
    void reorderTrack(const std::string& id, int newIndex);

    // Clip 操作
    std::string addClip(const std::string& trackId, const Clip& clip);
    void removeClip(const std::string& trackId, const std::string& clipId);
    void moveClip(const std::string& trackId, const std::string& clipId, int newTrackTick);
    void trimClip(const std::string& trackId, const std::string& clipId, int newInTick, int newOutTick);
    void splitClip(const std::string& trackId, const std::string& clipId, int atTick);
    void rippleDelete(const std::string& trackId, const std::string& clipId);

    // 转场
    std::string addTransition(const std::string& fromClipId, const std::string& toClipId, TransitionKind kind, int durationTicks);
    void removeTransition(const std::string& transitionId);

    // 查询
    std::vector<Clip*> getActiveClipsAt(int timelineTick, const TrackManager& self);
    const Transition* findTransitionBetween(const std::string& fromClipId, const std::string& toClipId) const;

private:
    mutable std::mutex  mMtx;
    EditorState         mState;
};
```

### 2.3 `Clip` 操作算法

#### Add

```cpp
std::string TrackManager::addClip(const std::string& trackId, const Clip& clip) {
    std::scoped_lock lk(mMtx);
    auto& track = findTrack(trackId);
    if (track.locked) return {};

    Clip c = clip;
    c.id = genUuid();
    c.color = pickColorFor(c.replayFile);  // 同源复用色
    track.clips.push_back(c);
    sortClipsByTick(track);
    return c.id;
}
```

#### Split

```cpp
void TrackManager::splitClip(const std::string& trackId, const std::string& clipId, int atTick) {
    std::scoped_lock lk(mMtx);
    auto& track = findTrack(trackId);
    auto it = findClipIter(track, clipId);
    if (it == track.clips.end() || track.locked || it->locked) return;

    int localTick = atTick - it->trackTick;  // 转回 clip 内部 tick
    if (localTick <= 0 || localTick >= (it->outTick - it->inTick)) return;

    Clip right = *it;
    right.id = genUuid();
    right.inTick = it->inTick + localTick;
    right.trackTick = it->trackTick + localTick;

    it->outTick = it->inTick + localTick;
    // right.outTick 不变

    track.clips.push_back(right);
    sortClipsByTick(track);
}
```

#### Trim In / Out

```cpp
void TrackManager::trimClip(const std::string& trackId, const std::string& clipId, int newInTick, int newOutTick) {
    auto& clip = findClip(trackId, clipId);
    if (clip.locked) return;

    int len = newOutTick - newInTick;
    if (len <= 0) return;

    // 边界检查：不与前 / 后 Clip 重叠（除非 force=true）
    clip.inTick = newInTick;
    clip.outTick = newOutTick;
    if (clip.outTick - clip.inTick <= 0) {  // rollback
        // restore
    }
}
```

#### Ripple Delete

```cpp
void TrackManager::rippleDelete(const std::string& trackId, const std::string& clipId) {
    auto& track = findTrack(trackId);
    auto& clip = findClip(track, clipId);

    int removeLen = clip.outTick - clip.inTick;
    auto it = findClipIter(track, clipId);
    track.clips.erase(it);

    // 后续 Clip 全部前移 removeLen
    for (auto& c : track.clips) {
        if (c.trackTick > clip.trackTick) {
            c.trackTick -= removeLen;
        }
    }
}
```

### 2.4 `TransitionEngine`（转场应用）

```cpp
class TransitionEngine {
public:
    // 调用于 RenderJob 每帧
    // 输入：当前 tick + 全部 clip，决定渲染哪些 + alpha
    struct RenderPlan {
        std::string primaryClipId;
        std::optional<std::string> secondaryClipId;  // 转场期间
        float blendAlpha{1.0f};  // 0=primary, 1=secondary
        TransitionKind kind{TransitionKind::Cut};
    };

    RenderPlan planAt(int timelineTick, const EditorState& editor);
};
```

**算法**：

```cpp
RenderPlan TransitionEngine::planAt(int timelineTick, const EditorState& editor) {
    RenderPlan plan;

    // 1) 找所有 active clips（按 timelineTick）
    std::vector<std::pair<int, Clip*>> active;
    for (auto& t : editor.videoTracks) {
        if (!t.visible || t.kind != TrackKind::Video) continue;
        for (auto& c : t.clips) {
            if (timelineTick >= c.trackTick && timelineTick < c.trackTick + (c.outTick - c.inTick)) {
                active.push_back({timelineTick - c.trackTick, &c});
            }
        }
    }
    if (active.empty()) return plan;
    if (active.size() == 1) {
        plan.primaryClipId = active[0].second->id;
        return plan;
    }

    // 2) 找转场
    auto& a = *active[0].second;
    auto& b = *active[1].second;
    auto* trans = findTransitionBetween(a.id, b.id);
    if (!trans) {
        plan.primaryClipId = a.id;
        return plan;
    }

    // 3) 在转场区间内混合
    int transStart = b.trackTick - trans->durationTicks;  // 转场从 a 末尾前开始
    int transEnd = b.trackTick;
    if (timelineTick < transStart || timelineTick > transEnd) {
        plan.primaryClipId = a.id;
        return plan;
    }
    int tickInTrans = timelineTick - transStart;
    plan.primaryClipId = a.id;
    plan.secondaryClipId = b.id;
    plan.kind = trans->kind;

    switch (trans->kind) {
        case TransitionKind::Cut:
            plan.blendAlpha = timelineTick < transEnd ? 0 : 1;
            break;
        case TransitionKind::Fade:
            // 仅 a 淡出 / b 不淡入（可配）
            plan.blendAlpha = easingValue(trans->easing, float(tickInTrans) / trans->durationTicks);
            break;
        case TransitionKind::CrossDissolve:
            float t = float(tickInTrans) / trans->durationTicks;
            float e = easingValue(trans->easing, t);
            plan.blendAlpha = e;  // 0=全 a, 1=全 b
            break;
    }
    return plan;
}
```

**渲染层应用**（详见 [05-render-pipeline.md](05-render-pipeline.md)）：`plan.secondaryClipId` 存在时，RenderJob **先渲 primary** 到 RTV-A，**再渲 secondary** 到 RTV-B，按 `blendAlpha` 把 B 混合到 A，写一帧到 pipe。

> **MVP 简化**：v1 一次只渲一个 Clip（取主）；转场在 04v1.1 再加。文档先写完整架构。

### 2.5 `BezierCurveEditor`（曲线编辑器）

```cpp
class BezierCurveEditor {
public:
    // 加载曲线 + 当前采样值
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

**UI 草图**：

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

**绘制**（De Casteljau 采样 64 段画线）：

```cpp
void BezierCurveEditor::draw(ImDrawList* dl, Rect area) {
    dl->AddRect(area.min, area.max, IM_COL32(100,100,100,255));
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 64; ++i) {
        float t = i / 64.0f;
        float y = sampleAt(t);
        pts.push_back({area.min.x + area.GetWidth() * t,
                       area.max.y - area.GetHeight() * y});
    }
    for (size_t i = 1; i < pts.size(); ++i)
        dl->AddLine(pts[i-1], pts[i], IM_COL32(255,200,0,255), 2.0f);

    // 控制点
    for (auto& p : mCurve.points) {
        ImVec2 c{area.min.x + area.GetWidth() * p.t, area.max.y - area.GetHeight() * p.v};
        dl->AddCircleFilled(c, 5.0f, IM_COL32(255,100,0,255));
    }
}
```

**采样**（Newton-Raphson 反函数）：

```cpp
float BezierCurveEditor::sampleAt(float t) const {
    if (mCurve.points.size() < 2) return t;
    // 1) 找到包含 t 的段
    auto seg = locateSegment(mCurve.points, t);
    if (!seg) return t;
    // 2) 段内局部 u
    auto& a = mCurve.points[seg->lo];
    auto& b = mCurve.points[seg->hi];
    float u = (t - a.t) / (b.t - a.t);
    // 3) Newton-Raphson 求 x(u) = u_local → v
    return bezierYFromX(u, a, b);
}
```

### 2.6 `SelectionModel`（选中模型）

```cpp
struct Selection {
    std::vector<std::string> clipIds;
    std::vector<std::string> trackIds;
    std::vector<std::string> keyframeIds;
    std::vector<std::string> transitionIds;
    int anchorTick{0};
};

class SelectionModel {
public:
    void select(const std::string& id, bool additive = false);
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

### 2.7 `EditingCommands`（Command 模式）

```cpp
struct IEditCommand {
    virtual ~IEditCommand() = default;
    virtual void execute(EditorState& s) = 0;
    virtual void undo(EditorState& s) = 0;
    virtual std::string label() const = 0;  // for menu
};

class AddClipCommand : public IEditCommand {
    std::string trackId;
    Clip clip;
public:
    void execute(EditorState& s) override { /* addClip */ }
    void undo(EditorState& s) override { /* removeClip */ }
    std::string label() const override { return "Add Clip"; }
};

class SplitClipCommand : public IEditCommand {
    std::string trackId, clipId;
    int atTick;
    Clip right;  // 保存 split 出的 right clip
public:
    void execute(EditorState& s) override { /* splitClip */ }
    void undo(EditorState& s) override { /* remove right + restore left.outTick */ }
    std::string label() const override { return "Split Clip at " + std::to_string(atTick); }
};

// ... AddTransitionCommand, TrimClipCommand, MoveClipCommand, ...
```

**CommandStack**（在 [01-editor-architecture.md](01-editor-architecture.md) 中详述）：

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

### 2.8 内部剪贴板

```cpp
class Clipboard {
public:
    void put(const std::vector<Clip>& clips, const std::vector<Transition>& transitions);
    std::vector<Clip>       getClips() const;
    std::vector<Transition> getTransitions() const;
private:
    std::vector<Clip>       mClips;
    std::vector<Transition> mTransitions;
};
```

**操作**：`Ctrl+X`（Cut）/ `Ctrl+C`（Copy）/ `Ctrl+V`（Paste）。粘贴时 tick = `playheadTick`。

### 2.9 时间轴渲染（参考，详细在 [01-editor-architecture.md](01-editor-architecture.md)）

```
+------------------------------------------------------------------+
|  ◀ ▶ ⏸  [00:01:23.456]  ↺ ↻  📍 [Marker]                       |
+------------------------------------------------------------------+
|  0:00      0:30      1:00      1:30      2:00                    |
|  |---------|---------|---------|---------|                        |
|  V2:    [   Clip A   ][   Clip B   ][   Clip C   ]                |
|  V1:        [Clip D   ]                                           |
|  C1: ═══════════●══════●═══════════●═══════ (摄影机主轨)         |
|  C0:        ══════●═══════════════●═════════  (摄影机副轨)        |
|  M:                  ◆ Boss        ◆ End                         |
+------------------------------------------------------------------+
```

**视觉**：
- 视频轨：横向矩形，颜色按 `.playback` 源 hash 着色
- 摄影机轨：横向细线 + 关键帧点 ◆
- 标记轨：◆ + 名字标签
- 拖拽：Clip 头/尾拖 → Trim；Clip 体拖 → Move
- 分割线：playhead 垂直黄线

### 2.10 与摄影机的集成

`Clip.activeCameraTrackIdx` 决定该段用哪条摄影机轨；切换到 Track 边界时，`CameraSystem` 自动切到下一 Clip 的摄影机。

```cpp
CameraSample CameraSystem::sampleAt(int tick, const ReplaySession& session) const {
    // 1) 找当前 active Clip
    auto* clip = findActiveClip(tick);
    int trackIdx = clip ? clip->activeCameraTrackIdx : mState.activeCameraTrackIdx;
    // 2) 用对应摄影机轨采样
    return sampleTrack(mState.tracks[trackIdx], tick, session);
}
```

## 三、执行（Execution）

### 3.1 任务拆分

| 步骤 | 文件 | 验证 |
|---|---|---|
| 1 | `SelectionModel.{h,cpp}` | 编译 |
| 2 | `Clipboard.{h,cpp}` | 编译 |
| 3 | `TrackManager.addClip / removeClip / moveClip` | 单测：增删改 |
| 4 | `TrackManager.trimClip / splitClip / rippleDelete` | 单测：5 个操作 |
| 5 | `TransitionEngine.planAt` | 单测：Cut/Fade/CrossDissolve |
| 6 | `BezierCurve` 采样 + 序列化（已在 [06-data-persistence.md](06-data-persistence.md)） | 单测：5 个采样点 |
| 7 | `BezierCurveEditor` UI 草图 | 手动：手画曲线 |
| 8 | `EditingCommands`（AddClip / Split / Trim / Move / AddTransition） | 单测：execute + undo 互逆 |
| 9 | `CommandStack` | 单测：100 步 undo 不丢 |
| 10 | 时间轴 UI 草图 | 手动：拖 Clip / 关键帧 |
| 11 | 与 `CameraSystem` 集成 | 手动：切换 Clip 摄影机切 |
| 12 | 与 `RenderJob` 集成 | 手动：导出包含转场 |

### 3.2 关键算法

**Clip 排序**（按 trackTick 升序）：

```cpp
void sortClipsByTick(Track& track) {
    std::sort(track.clips.begin(), track.clips.end(),
              [](const Clip& a, const Clip& b){ return a.trackTick < b.trackTick; });
}
```

**转场应用**（渲染层伪代码）：

```cpp
void RenderJob::renderFrameWithTransition(int timelineTick) {
    auto plan = TransitionEngine::planAt(timelineTick, mEditor);

    // 1) 渲 primary 到 RTV
    mFrameSource->beginFrame();
    mReplay->seek(plan.primaryClip().inTick + (timelineTick - plan.primaryClip().trackTick));
    mReplay->tick();
    CameraSystem::sampleAndApply(plan.primaryClip().activeCameraTrackIdx, ...);
    mFrameSource->waitForFrame();
    mFrameSource->captureToStaging(rgbA);

    if (!plan.secondaryClipId) {
        mEncoder->writeVideoFrame(rgbA, ...);
        return;
    }

    // 2) 渲 secondary 到 RTV-B
    mFrameSourceB->beginFrame();
    mReplay->seek(plan.secondaryClip().inTick + ...);
    mReplay->tick();
    CameraSystem::sampleAndApply(plan.secondaryClip().activeCameraTrackIdx, ...);
    mFrameSourceB->waitForFrame();
    mFrameSourceB->captureToStaging(rgbB);

    // 3) alpha 混合 rgbA ← rgbB * alpha
    blend(rgbA, rgbB, plan.blendAlpha);
    mEncoder->writeVideoFrame(rgbA, ...);
}
```

> **MVP**：v1 不实现双 RTV；转场 = 抽帧混合，落到单 RTV。完整双 RTV 在 04v1.1。

### 3.3 关键不变量

1. **Clip 不重叠**（除转场区）：UI 自动避免重叠，否则报 warning
2. **Track 排序稳定**：按 index，UI 上手动 reorder
3. **transition 必须连接两个 Clip**：`fromClipId.outTick + durationTicks == toClipId.trackTick`
4. **Command execute / undo 互逆**：undo 状态 == 执行前状态（property test）
5. **BezierCurve 单调 x**：x 严格升序，避免多解
6. **Selection 唯一**：UI 上只显示一个"主选中"（其余浅色）

### 3.4 测试用例

| ID | 用例 | 期望 |
|---|---|---|
| VE-T1 | addClip + sortClipsByTick | tick 升序 |
| VE-T2 | splitClip(中点) | 两个 clip，tick 接续 |
| VE-T3 | trimClip(缩短) | inTick 增加，len 减 |
| VE-T4 | rippleDelete | 后续 clip trackTick 前移 |
| VE-T5 | addTransition(Cut, dur=0) | 立即切 |
| VE-T6 | addTransition(Fade, dur=20) | 20 tick 渐变 |
| VE-T7 | planAt 在转场中点 | blendAlpha = 0.5 |
| VE-T8 | planAt 转场前 | primary |
| VE-T9 | undo AddClipCommand | clip 消失 |
| VE-T10 | 100 步 undo + redo | 栈不丢 |
| VE-T11 | BezierCurve 端点 | sample(0)=0, sample(1)=1 |
| VE-T12 | Clip 拖出 Track 边界 | 警告 / clamp |

### 3.5 风险与回退

| 风险 | 缓解 |
|---|---|
| 双 RTV 性能 | 首期单 RTV + 抽帧 |
| 撤销栈内存膨胀 | maxUndoSteps 截断 |
| 转场与 Trim 冲突 | UI 提示"转场将被删除" |
| Bezier 手画 UX 难 | 拖拽 + 关键点直接选；可改回纯 easing |

## 四、模块关系

### 被谁调用（上游）

- **`refactor/editor-architecture/Panels/TimelinePanel`**：调所有 Clip / Track / Transition 操作
- **`refactor/editor-architecture/Panels/CurveEditorPanel`**：调 BezierCurveEditor
- **`refactor/editor-architecture/CommandStack`**：包装所有 EditingCommands
- **`refactor/render-pipeline/RenderJob`**：调 `TransitionEngine::planAt` 拿转场计划

### 调用谁（下游）

- **旧 [EditorContext](../editor/context/EditorContext.md)**：读写 `EditorState`
- **[02-camera-motion.md](02-camera-motion.md)**：Clip.activeCameraTrackIdx 切摄影机
- **[06-data-persistence.md](06-data-persistence.md)**：数据模型
- **旧 [ReplaySession](../functions/replay.md)**：seek 到 Clip.inTick + offset

### 共享数据

- `EditorContext::mEditorExt` —— UI ↔ 数据
- `TrackManager::mState`（私有，写者唯一）

### 事件订阅 / 发送

- 无

## 五、阅读顺序

1. 本文件
2. [06-data-persistence.md](06-data-persistence.md) —— 数据模型
3. [02-camera-motion.md](02-camera-motion.md) —— 摄影机
4. [05-render-pipeline.md](05-render-pipeline.md) —— 渲染消费
5. [01-editor-architecture.md](01-editor-architecture.md) —— UI
