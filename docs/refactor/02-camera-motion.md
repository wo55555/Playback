# 02 · 摄像机与运动控制

> 入口：`src/playback/refactor/camera-motion/`
> 角色：在旧 [CameraTrack](../editor/camera-track.md)（关键帧 + easing）基础上，扩展为 **多轨 / 3D 样条 / 8 种运动原语 / 摄影机预设 / 绑定+阻尼 / Shake / 限位器 / 时间重映射 / 标记** 的电影级摄影机系统。
> 数据模型见 [06-data-persistence.md](06-data-persistence.md)；本文件描述**算法与运行时行为**。

## 一、需求（Requirements）

### 1.1 功能性需求

| ID | 需求 | 优先级 |
|---|---|---|
| CM-1 | 多条摄影机轨道平行存在，导出 / 预览可任选其一作为主轨 | P0 |
| CM-2 | 4 种摄影机类型：**Keyframe**（旧）、**Path**（3D 样条）、**Rig**（运动原语）、**Preset**（预设） | P0 |
| CM-3 | 3D 样条路径支持 Linear / Catmull-Rom / CubicBezier，可拖拽控制点 | P0 |
| CM-4 | 8 种运动原语：Dolly / Truck / Pedestal / Pan / Tilt / Roll / Zoom / Follow | P0 |
| CM-5 | 7 种摄影机预设：FirstPerson / ThirdPerson / Free / FollowEntity / Orbit / Telephoto / Drone | P0 |
| CM-6 | 摄影机可绑定到实体（玩家 / 自定义 UUID），支持位置 / 角度 / 全跟随，弹簧阻尼 | P0 |
| CM-7 | Camera Shake：Perlin / Simplex 噪声驱动，可叠加在任意摄影机类型上 | P0 |
| CM-8 | 限位器：Box / Sphere / PlayerAABB；防穿模 / 防出界 | P0 |
| CM-9 | Time Remap：speed curve，任意时间点变速 / 慢动作 | P0 |
| CM-10 | 标记（Marker）：命名书签 + 颜色 + note | P0 |
| CM-11 | 采样是**纯函数**：相同 tick 多次采样结果一致（确定性） | P0 |
| CM-12 | 摄影机状态可被 [RenderJob](05-render-pipeline.md) 每帧采样并注入 MCBE CameraManager | P0 |

### 1.2 非功能性需求

- **采样延迟**：单轨道 tick 采样 < 0.5ms（含 8 段 Rig + 1 个 Shake + 1 个 Limiter）
- **抖动开销**：Perlin 噪声调用 < 0.1ms / tick（用 5-tick 缓存）
- **关键帧数**：单轨道支持 ≤ 1024 关键帧（locateSegment O(log n)）
- **内存**：单 CameraTrackExt 完整字段 ≤ 8KB
- **线程安全**：`sampleAt()` 是 `const`，多线程只读安全

### 1.3 与现有约束对齐

- 复用旧 [CameraTrack](../editor/camera-track.md) 的关键帧 + easing
- 复用 [EditorContext](../editor/context/EditorContext.md) 的 mutex 模式
- 复用 MCBE `CameraManager` 内部字段写入路径
- 摄影机注入点仅在 `__playback_replay_world__`（隔离世界）启用

## 二、架构（Architecture）

### 2.1 内部结构

```
refactor/camera-motion/
├── CameraSystem.h / .cpp             ← 顶层协调器
├── CameraTrackExt.{h,cpp}            ← 多轨 + 4 种 kind 封装
├── CameraPath.{h,cpp}                ← 3D 样条
├── CameraRig.{h,cpp}                 ← 运动原语
├── CameraPreset.{h,cpp}              ← 预设
├── CameraShake.{h,cpp}               ← 抖动
├── CameraLimiter.{h,cpp}             ← 限位
├── CameraBinder.{h,cpp}              ← 实体绑定 + 阻尼
├── CameraSampler.h                   ← tick → CameraSample（聚合所有效果）
├── SplineMath.h / .cpp               ← 通用样条数学
└── PerlinNoise.h / .cpp              ← Perlin/Simplex（stb_perlin 包装）
```

### 2.2 `CameraSystem`（顶层协调器）

```cpp
class CameraSystem {
public:
    static CameraSystem& getInstance();

    // 配置（由 EditorContext 喂入）
    void setEditorState(const EditorState& state);
    const EditorState& snapshot() const;

    // 主入口：每 tick 调一次
    CameraSample sampleAt(int tick, const ReplaySession& session) const;

    // 摄影机注入 MCBE（每帧调一次；只对 EditorRenderMode 生效）
    void applyToMCBE(const CameraSample& s) const;

private:
    mutable std::mutex    mMtx;
    EditorState           mState;
    mutable LRUCache<int, CameraSample> mCache{1024};  // tick → sample
};
```

### 2.3 `CameraSampler`（tick → CameraSample 聚合）

```cpp
struct CameraSample {
    Vec3       position{0, 80, 0};
    Vec2       rotation{0, 0};
    float      fov{90.0f};
    std::string source;  // "Main/CameraRig/Dolly" 等（用于调试）
    bool       valid{true};
};

CameraSample CameraSampler::sampleAt(
    const EditorState& editor,
    int tick,
    const ReplaySession& session   // 拿玩家位置 / 实体位置
) {
    if (editor.tracks.empty()) return {};

    const auto& track = editor.tracks[editor.activeCameraTrackIdx];
    if (!track.visible) return {};

    // 1) 基础：按 kind 采样
    CameraSample s = {};
    switch (track.kind) {
        case CameraKind::Keyframe: s = sampleKeyframes(track.keys, tick); break;
        case CameraKind::Path:     s = samplePath(*track.path, tick); break;
        case CameraKind::Rig:      s = sampleRig(*track.rig, track.preset, tick); break;
        case CameraKind::Preset:   s = samplePreset(*track.preset, tick, session); break;
    }

    // 2) 实体绑定 + 阻尼（覆写 position / rotation）
    if (!track.bindingEntityUuid.empty()) {
        s = applyBinding(s, track, tick, session);
    }

    // 3) Shake 叠加
    for (const auto& sh : track.shakes) {
        if (tick >= sh.startTick && tick <= sh.endTick) {
            s.position += shakeOffsetAt(sh, tick, 0);
            s.rotation += shakeRotationAt(sh, tick, 1);
        }
    }

    // 4) Limiter clamp
    if (track.limiter) {
        s.position = clampByLimiter(s.position, *track.limiter, session);
    }

    s.source = track.name;
    return s;
}
```

### 2.4 Keyframe 采样（沿用旧逻辑 + 扩展）

```cpp
CameraSample sampleKeyframes(const std::vector<CameraKeyframe>& keys, int tick) {
    if (keys.empty()) return {};
    if (tick <= keys.front().tick) return {keys.front().position, keys.front().rotation, keys.front().fov};
    if (tick >= keys.back().tick)  return {keys.back().position,  keys.back().rotation,  keys.back().fov};

    auto seg = locateSegment(keys, tick);
    const auto& a = keys[seg.lo];
    const auto& b = keys[seg.hi];
    float t = float(tick - a.tick) / float(b.tick - a.tick);
    return {
        lerpByEasing(a.position, b.position, t, a.easing),
        lerpByEasing(a.rotation, b.rotation, t, a.easing),
        lerpByEasing(a.fov,       b.fov,       t, a.easing),
    };
}
```

> 详见旧 [camera-track.md §2.3 采样算法](../editor/camera-track.md)。

### 2.5 Path 采样（3D 样条）

```cpp
CameraSample samplePath(const CameraPath& path, int tick) {
    if (path.points.empty()) return {};
    if (tick <= path.points.front().tick)
        return {path.points.front().position, path.defaultRotation, path.defaultFov};
    if (tick >= path.points.back().tick)
        return {path.points.back().position, lastRotation(path), path.defaultFov};

    auto seg = locateSegment(path.points, tick);
    float t = float(tick - path.points[seg.lo].tick) / float(path.points[seg.hi].tick - path.points[seg.lo].tick);

    Vec3 pos;
    switch (path.type) {
        case SplineType::Linear:
            pos = lerp(path.points[seg.lo].position, path.points[seg.hi].position, t);
            break;
        case SplineType::CatmullRom:
            pos = catmullRom(
                path.points[std::max(0, (int)seg.lo - 1)].position,
                path.points[seg.lo].position,
                path.points[seg.hi].position,
                path.points[std::min((int)path.points.size()-1, (int)seg.hi + 1)].position,
                t
            );
            break;
        case SplineType::CubicBezier:
            pos = cubicBezier(
                path.points[seg.lo].position,
                path.points[seg.lo].outTangent,
                path.points[seg.hi].inTangent,
                path.points[seg.hi].position,
                t
            );
            break;
    }

    // tangent → 旋转
    Vec3 forward = pos - previousPointOnPath(path, seg.lo);  // 简化
    Vec2 rot = lookAtToYawPitch(forward);

    return {pos, rot, path.defaultFov};
}
```

**Catmull-Rom 公式**（4 点，t ∈ [0,1]）：

```
P(t) = 0.5 * (
    (2*p1) +
    (-p0 + p2) * t +
    (2*p0 - 5*p1 + 4*p2 - p3) * t² +
    (-p0 + 3*p1 - 3*p2 + p3) * t³
)
```

**Cubic Bezier De Casteljau**：

```cpp
Vec3 cubicBezier(Vec3 p0, Vec3 c1, Vec3 c2, Vec3 p3, float t) {
    Vec3 a = lerp(p0, c1, t);
    Vec3 b = lerp(c1, c2, t);
    Vec3 c = lerp(c2, p3, t);
    Vec3 d = lerp(a, b, t);
    Vec3 e = lerp(b, c, t);
    return lerp(d, e, t);
}
```

### 2.6 Rig 采样（运动原语叠加）

```cpp
CameraSample sampleRig(const CameraRig& rig, const std::optional<CameraPreset>& preset, int tick) {
    CameraSample s = {rig.basePosition, rig.baseRotation, rig.baseFov};

    for (const auto& seg : rig.segments) {
        if (tick < seg.startTick || tick > seg.endTick) continue;
        float t = float(tick - seg.startTick) / float(seg.endTick - seg.startTick);
        float eased = easingValue(seg.easing, t);
        float delta = seg.startValue + (seg.endValue - seg.startValue) * eased;

        switch (seg.motion) {
            case RigMotion::Dolly:    s.position += forwardVec(s.rotation) * delta; break;
            case RigMotion::Truck:    s.position += rightVec(s.rotation)   * delta; break;
            case RigMotion::Pedestal: s.position.y += delta; break;
            case RigMotion::Pan:      s.rotation.x += delta; break;
            case RigMotion::Tilt:     s.rotation.y += delta; break;
            case RigMotion::Roll:     /* 应用于 CameraManager.roll */ break;
            case RigMotion::Zoom:     s.fov += delta; break;
            case RigMotion::Follow:   /* 由 applyBinding 处理 */ break;
            default: break;
        }
    }

    return s;
}
```

> Roll 是绕前向轴旋转（UE5 / Blender 的"roll"）；MCBE 内部 `CameraManager` 有对应字段。

### 2.7 Preset 采样（7 种预设）

```cpp
CameraSample samplePreset(const CameraPreset& p, int tick, const ReplaySession& s) {
    Vec3 anchor = s.getPlayerPosition();  // FirstPerson / ThirdPerson / FollowEntity 用
    Vec2 rot = p.rotation;

    switch (p.kind) {
        case PresetKind::FirstPerson:
            return {anchor, s.getPlayerRotation(), p.fov};
        case PresetKind::ThirdPerson:
            return {anchor + backVec(s.getPlayerRotation()) * 4.f + Vec3{0, 2, 0}, s.getPlayerRotation(), p.fov};
        case PresetKind::Free:
            return {p.offset, p.rotation, p.fov};
        case PresetKind::FollowEntity: {
            auto* entity = s.findEntityByUuid(p.bindingEntityUuid);
            if (!entity) return {p.offset, p.rotation, p.fov};
            return {entity->getPosition() + p.offset, entity->getRotation() + p.rotation, p.fov};
        }
        case PresetKind::Orbit: {
            float phase = (tick + p.orbitPhaseTick) * 0.05f * p.orbitSpeed;
            Vec3 center = p.orbitCenter;
            return {
                center + Vec3{std::cos(phase) * p.orbitRadius, p.offset.y, std::sin(phase) * p.orbitRadius},
                rot, p.fov
            };
        }
        case PresetKind::Telephoto:
            return {p.offset, p.rotation, 30.0f};  // 固定窄 FOV
        case PresetKind::Drone: {
            // 鸟瞰：offset.y 大，fov 中等
            return {p.offset, p.rotation, p.fov};
        }
    }
}
```

### 2.8 实体绑定 + 阻尼

```cpp
CameraSample applyBinding(const CameraSample& base, const CameraTrackExt& track,
                           int tick, const ReplaySession& session) {
    Vec3 targetPos = base.position;
    Vec2 targetRot = base.rotation;

    // 1) 拿绑定目标位置
    Vec3 entityPos = session.getPlayerPosition();
    Vec2 entityRot = session.getPlayerRotation();
    if (!track.bindingEntityUuid.empty()) {
        auto* e = session.findEntityByUuid(track.bindingEntityUuid);
        if (e) { entityPos = e->getPosition(); entityRot = e->getRotation(); }
    }

    // 2) 按 mode 混合
    switch (track.bindingMode) {
        case 1: targetPos = entityPos + base.position; break;  // 位置跟随
        case 2: targetRot = entityRot + base.rotation; break;  // 角度跟随
        case 3: targetPos = entityPos + base.position;
                targetRot = entityRot + base.rotation; break;  // 全跟随
        default: break;
    }

    // 3) 弹簧阻尼（保持上一帧 + 平滑过渡）
    auto& prev = mPrevFrame[track.name];  // 缓存上一帧
    if (prev.valid) {
        float k = 1.0f - std::clamp(track.bindingDamping, 0.0f, 1.0f);
        targetPos = prev.position + (targetPos - prev.position) * k;
        targetRot = lerpAngle(prev.rotation, targetRot, k);
    }
    prev = {targetPos, targetRot, base.fov, true};

    return {targetPos, targetRot, base.fov};
}
```

**关键不变量**：每帧 `mPrevFrame` 更新；采样是**非纯函数**（依赖上一帧），但导出 / 预览行为一致（因为初始 prev = invalid，从头跑）。

### 2.9 Shake（抖动）

```cpp
Vec3 shakeOffsetAt(const CameraShake& s, int tick, int seed) {
    // 5-tick 缓存
    int bucket = tick / 5;
    if (mShakeCache.contains(bucket)) return mShakeCache[bucket].pos;

    float t = (tick - s.startTick) / float(s.endTick - s.startTick);
    if (t < 0 || t > 1) return {};
    float decay = easingValue(s.decay, t);
    float time = tick * s.frequency * 0.01f;

    Vec3 v = {
        perlin(s.positionSeed.x + time, seed) * s.amplitude * decay,
        perlin(s.positionSeed.y + time, seed) * s.amplitude * decay,
        perlin(s.positionSeed.z + time, seed) * s.amplitude * decay
    };
    mShakeCache[bucket] = {v, 0};
    return v;
}
```

`perlin` 由 [stb_perlin.h](https://github.com/nothings/stb) 提供，封装在 `PerlinNoise.h`。

### 2.10 Limiter（限位）

```cpp
Vec3 clampByLimiter(const Vec3& pos, const CameraLimiter& l, const ReplaySession& s) {
    switch (l.shape) {
        case LimiterShape::Box: {
            AABB b = l.box;
            if (l.followPlayer) b.center += s.getPlayerPosition();
            return Vec3{
                std::clamp(pos.x, b.min.x + l.padding.x, b.max.x - l.padding.x),
                std::clamp(pos.y, b.min.y + l.padding.y, b.max.y - l.padding.y),
                std::clamp(pos.z, b.min.z + l.padding.z, b.max.z - l.padding.z)
            };
        }
        case LimiterShape::Sphere: {
            Vec3 c = l.followPlayer ? l.sphereCenter + s.getPlayerPosition() : l.sphereCenter;
            Vec3 d = pos - c;
            float len = d.length();
            if (len > l.sphereRadius) return c + d * (l.sphereRadius / len);
            return pos;
        }
        case LimiterShape::PlayerAABB: {
            Vec3 pp = s.getPlayerPosition();
            AABB b{pp - Vec3{32, 0, 32}, pp + Vec3{32, 256, 32}};
            return Vec3{
                std::clamp(pos.x, b.min.x, b.max.x),
                std::clamp(pos.y, b.min.y, b.max.y),
                std::clamp(pos.z, b.min.z, b.max.z)
            };
        }
    }
}
```

### 2.11 Time Remap

```cpp
// 在 RenderJob 主循环：
int sourceTick = renderedFrame * ticksPerFrame;  // 假设 fps=60, ticksPerSec=20 → 步进 0.33
int remappedTick = editor.timeRemap.remap(sourceTick);
session.requestSeek(remappedTick);
```

详见 [06-data-persistence.md §2.10](06-data-persistence.md)。

### 2.12 MCBE CameraManager 注入

```cpp
void CameraSystem::applyToMCBE(const CameraSample& s) const {
    if (!ReplaySession::getInstance().isEditorRenderMode()) return;

    auto* camMgr = ClientInstance::get()->getCameraManager();
    camMgr->mCurrentCameraPosition = s.position;
    camMgr->mCurrentCameraRotation = s.rotation;
    camMgr->mFov = s.fov;
    // Roll 字段
    if (s.rotation.z != 0) camMgr->mRoll = s.rotation.z;
}
```

> **风险**：MCBE 内部字段偏移变化；用 [tooth.json](file:///d:/raplay/Playback/tooth.json) 锁基线版本，启动时校验。

### 2.13 Viewport 内 gizmo

摄影机在 3D 视口内显示为 **gizmo**（位置/旋转/FOV 锥），可拖拽编辑：

- **位置 gizmo**：3 轴箭头（X 红 / Y 绿 / Z 蓝）
- **旋转 gizmo**：3 环（pitch / yaw / roll）
- **FOV gizmo**：矩形框

gizmo 渲染用 [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) 或自写；拖拽时实时改 `CameraKeyframe` / `SplineControlPoint`。

> **属于 UI 层**，详见 [01-editor-architecture.md](01-editor-architecture.md) 后续设计。

## 三、执行（Execution）

### 3.1 任务拆分

| 步骤 | 文件 | 验证 |
|---|---|---|
| 1 | `SplineMath.{h,cpp}` | 单测：3 种 spline 已知点 |
| 2 | `PerlinNoise.h` 包装 stb_perlin | 单测：perlin(0) ≈ 0 |
| 3 | `CameraPath.{h,cpp}` + 序列化 | 单测：locateSegment / sampleAt |
| 4 | `CameraRig.{h,cpp}` + 序列化 | 单测：8 种 motion 各跑 1 段 |
| 5 | `CameraPreset.{h,cpp}` + 序列化 | 单测：7 种 preset 输出 |
| 6 | `CameraShake.{h,cpp}` + 序列化 | 单测：噪声范围 |
| 7 | `CameraLimiter.{h,cpp}` + 序列化 | 单测：3 种 shape clamp |
| 8 | `CameraBinder.{h,cpp}` 阻尼 | 单测：连续帧平滑 |
| 9 | `CameraSampler.h` 聚合 | 单测：tick=mid 输出 |
| 10 | `CameraTrackExt` 封装 | 编译 |
| 11 | `CameraSystem` 单例 + 缓存 | 单测：5-tick 缓存命中 |
| 12 | MCBE `applyToMCBE` | 手动：隔离世界内切换 |
| 13 | 集成 `RenderJob` 采样 | 手动：导出摄影机位置正确 |

### 3.2 关键算法

**Catmull-Rom 矩阵**（4 点 p0,p1,p2,p3；输出 t=0..1）：

```cpp
Vec3 catmullRom(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * (
        (2.0f * p1) +
        (-p0 + p2) * t +
        (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3) * t2 +
        (-p0 + 3.0f*p1 - 3.0f*p2 + p3) * t3
    );
}
```

**弹簧阻尼**（简化版）：

```cpp
Vec3 damped(Vec3 prev, Vec3 target, float damping01) {
    return prev + (target - prev) * (1.0f - std::clamp(damping01, 0.0f, 1.0f));
}
```

**角度插值**（yaw 不走线性）：

```cpp
Vec2 lerpAngle(Vec2 a, Vec2 b, float t) {
    Vec2 d = b - a;
    // yaw ∈ (-π, π]
    if (d.x >  M_PI) d.x -= 2*M_PI;
    if (d.x < -M_PI) d.x += 2*M_PI;
    return a + d * t;
}
```

### 3.3 关键不变量

1. **sampleAt 纯函数（除 binding）**：除实体绑定外，所有采样**不依赖**全局状态
2. **绑定阻尼有状态**：每帧 `mPrevFrame` 缓存；导出 / 预览从头跑，结果一致
3. **Shake 5-tick 缓存**：避免每帧 Perlin 调用
4. **样条端点不外推**：tick < 起点 → 起点；tick > 终点 → 终点
5. **MCBE 注入仅隔离世界**：避免污染在线服务器

### 3.4 测试用例

| ID | 用例 | 期望 |
|---|---|---|
| CM-T1 | CatmullRom(t=0) | 等于 p1 |
| CM-T2 | CatmullRom(t=1) | 等于 p2 |
| CM-T3 | CubicBezier 端点 | p0 / p3 |
| CM-T4 | Dolly +5 在 rig 上 | position += forward * 5 |
| CM-T5 | FollowEntity 不存在 | fallback 到 base |
| CM-T6 | Shake 端点 | 0 振幅 |
| CM-T7 | Limiter Box 出界 | clamp 到 box |
| CM-T8 | 绑定连续 10 帧 | 平滑过渡 |
| CM-T9 | 256 关键帧 + 1 shake + 1 limiter | sample < 0.5ms |
| CM-T10 | EditorRenderMode 关闭时 applyToMCBE | no-op |

### 3.5 风险与回退

| 风险 | 缓解 |
|---|---|
| 样条控制点拖拽冲突 | 锁定到 tick 网格（drag 期间 tick 不变） |
| 绑定阻尼初始帧抖动 | 第一帧 prev = invalid，target = base，无插值 |
| MCBE 字段偏移变化 | tooth.json 锁基线 + 启动符号校验 |
| Orbit 数学溢出 | tick wrap 每 360° 一次 |
| 多摄影机轨导出时切换 | RenderJob 选 activeCameraTrackIdx，其它轨仍采样（不渲染） |

## 四、模块关系

### 被谁调用（上游）

- **`refactor/render-pipeline/RenderJob`**：每帧 `sampleAt` 拿当前摄影机
- **`refactor/render-pipeline/RealtimePreview`**：实时预览每帧采样
- **`refactor/editor-architecture/Panels/CameraTrackPanel`**：编辑摄影机轨道
- **`refactor/editor-architecture/Panels/ViewportPanel`**：显示 gizmo + 拖拽编辑

### 调用谁（下游）

- **旧 [EditorContext](../editor/context/EditorContext.md)**：读 `EditorState` 快照
- **旧 [ReplaySession](../functions/replay.md)**：拿玩家位置 / 实体位置
- **MCBE `CameraManager`**：注入摄影机
- **stb_perlin.h**（新增）：噪声
- **[06-data-persistence.md](06-data-persistence.md)**：数据模型

### 共享数据

- `CameraSystem::mState` —— 摄影机状态
- `EditorContext::mCameraExt` —— UI ↔ 数据
- `mPrevFrame`（私有）—— 阻尼上一帧

### 事件订阅 / 发送

- 无

## 五、阅读顺序

1. 本文件
2. [06-data-persistence.md](06-data-persistence.md) —— 数据模型
3. [05-render-pipeline.md](05-render-pipeline.md) —— 渲染消费
4. [01-editor-architecture.md](01-editor-architecture.md) —— UI 编辑入口
