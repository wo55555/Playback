#include "CameraTimelineEvaluator.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace playback::editor::keyframe {

namespace {

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

float wrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

float lerpAngle(float left, float right, float amount) {
    return wrapDegrees(left + wrapDegrees(right - left) * amount);
}

float cubicBezier(float p0, float p1, float p2, float p3, float t) {
    float const inverse = 1.0f - t;
    return inverse * inverse * inverse * p0 + 3.0f * inverse * inverse * t * p1 + 3.0f * inverse * t * t * p2
         + t * t * t * p3;
}

float cubicBezierEase(float t, editing::model::Vec2 const& control1, editing::model::Vec2 const& control2) {
    t                = clampUnit(t);
    float const x1   = clampUnit(control1.x);
    float const x2   = clampUnit(control2.x);
    float       low  = 0.0f;
    float       high = 1.0f;
    for (int iteration = 0; iteration < 18; ++iteration) {
        float const middle = (low + high) * 0.5f;
        if (cubicBezier(0.0f, x1, x2, 1.0f, middle) < t) low = middle;
        else high = middle;
    }
    float const solved = (low + high) * 0.5f;
    return clampUnit(cubicBezier(0.0f, control1.y, control2.y, 1.0f, solved));
}

float eased(float t, editing::model::CameraKeyframe const& key) {
    t = clampUnit(t);
    switch (key.easingType) {
    case editing::model::EasingType::Hold:
        return 0.0f;
    case editing::model::EasingType::EaseIn:
        return t * t;
    case editing::model::EasingType::EaseOut:
        return 1.0f - (1.0f - t) * (1.0f - t);
    case editing::model::EasingType::EaseInOut:
        return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
    case editing::model::EasingType::CubicBezier:
        return cubicBezierEase(t, key.bezierCtrl1, key.bezierCtrl2);
    case editing::model::EasingType::Linear:
    default:
        return t;
    }
}

editing::model::Vec3
lerp(editing::model::Vec3 const& left, editing::model::Vec3 const& right, float amount) {
    return {
        left.x + (right.x - left.x) * amount,
        left.y + (right.y - left.y) * amount,
        left.z + (right.z - left.z) * amount,
    };
}

CameraRenderState stateFromKeyframe(editing::model::CameraKeyframe const& key) {
    return {
        key.position.x,
        key.position.y,
        key.position.z,
        key.yaw,
        key.pitch,
        0.0f,
        std::clamp(key.fov, 1.0f, 179.0f),
    };
}

} // namespace

CameraTimelineEvaluator::CameraTimelineEvaluator(
    editing::model::EditorStateExt project,
    std::optional<std::string>     cameraOverride
)
: mProject(std::move(project)),
  mCameraOverride(std::move(cameraOverride)) {
    for (auto& camera : mProject.cameras) {
        std::ranges::sort(camera.keys, {}, &editing::model::CameraKeyframe::tick);
        if (camera.path) std::ranges::sort(camera.path->points, {}, &editing::model::CameraPathPoint::tick);
    }
}

editing::model::CameraEntity const* CameraTimelineEvaluator::cameraForTick(int64_t tick) const {
    if (mProject.cameras.empty()) return nullptr;

    std::string cameraId;
    if (mCameraOverride) cameraId = *mCameraOverride;
    if (cameraId.empty()) {
        for (auto const& segment : mProject.sequence) {
            if (tick >= segment.startTick && tick < segment.endTick && !segment.cameraId.empty()) {
                cameraId = segment.cameraId;
                break;
            }
        }
    }
    if (cameraId.empty() && mProject.activeCameraIndex >= 0
        && static_cast<size_t>(mProject.activeCameraIndex) < mProject.cameras.size()) {
        cameraId = mProject.cameras[static_cast<size_t>(mProject.activeCameraIndex)].id;
    }
    if (cameraId.empty()) {
        auto const active = std::ranges::find(mProject.cameras, true, &editing::model::CameraEntity::active);
        if (active != mProject.cameras.end()) cameraId = active->id;
    }

    auto const match = std::ranges::find(mProject.cameras, cameraId, &editing::model::CameraEntity::id);
    if (match != mProject.cameras.end()) return &*match;
    return mCameraOverride ? nullptr : &mProject.cameras.front();
}

std::optional<CameraRenderState>
CameraTimelineEvaluator::sampleCamera(editing::model::CameraEntity const& camera, long double tick) const {
    if (!camera.keys.empty()) {
        if (tick <= camera.keys.front().tick) return stateFromKeyframe(camera.keys.front());
        if (tick >= camera.keys.back().tick) return stateFromKeyframe(camera.keys.back());

        auto const right = std::ranges::lower_bound(camera.keys, tick, {}, &editing::model::CameraKeyframe::tick);
        if (right == camera.keys.end()) return std::nullopt;
        if (tick == static_cast<long double>(right->tick)) return stateFromKeyframe(*right);
        if (right == camera.keys.begin()) return std::nullopt;

        auto const& prev = *(right - 1);
        auto const  span = static_cast<long double>(right->tick - prev.tick);
        float const raw  = span <= 0.0L ? 0.0f : static_cast<float>((tick - prev.tick) / span);
        float const t    = eased(raw, prev);
        auto const  pos  = lerp(prev.position, right->position, t);
        return CameraRenderState{
            pos.x,
            pos.y,
            pos.z,
            lerpAngle(prev.yaw, right->yaw, t),
            lerpAngle(prev.pitch, right->pitch, t),
            0.0f,
            std::clamp(prev.fov + (right->fov - prev.fov) * t, 1.0f, 179.0f),
        };
    }

    if (camera.path && !camera.path->points.empty()) {
        auto const& points   = camera.path->points;
        auto        position = points.front().position;
        if (tick >= points.back().tick) {
            position = points.back().position;
        } else if (tick > points.front().tick) {
            auto const right = std::ranges::lower_bound(points, tick, {}, &editing::model::CameraPathPoint::tick);
            if (right != points.begin() && right != points.end()) {
                auto const& prev   = *(right - 1);
                auto const  span   = static_cast<long double>(right->tick - prev.tick);
                float const amount = span <= 0.0L ? 0.0f : static_cast<float>((tick - prev.tick) / span);
                position           = lerp(prev.position, right->position, clampUnit(amount));
            }
        }
        return CameraRenderState{
            position.x,
            position.y,
            position.z,
            camera.path->defaultRotation.x,
            camera.path->defaultRotation.y,
            0.0f,
            std::clamp(camera.path->defaultFov, 1.0f, 179.0f),
        };
    }
    if (camera.rig) {
        return CameraRenderState{
            camera.rig->basePosition.x,
            camera.rig->basePosition.y,
            camera.rig->basePosition.z,
            camera.rig->baseRotation.x,
            camera.rig->baseRotation.y,
            0.0f,
            std::clamp(camera.rig->baseFov, 1.0f, 179.0f),
        };
    }
    if (camera.preset) {
        return CameraRenderState{
            camera.preset->offset.x,
            camera.preset->offset.y,
            camera.preset->offset.z,
            camera.preset->rotation.x,
            camera.preset->rotation.y,
            0.0f,
            std::clamp(camera.preset->fov, 1.0f, 179.0f),
        };
    }
    return std::nullopt;
}

std::optional<CameraRenderState>
CameraTimelineEvaluator::sample(functions::render::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto const* camera = cameraForTick(time.floorTick());
    return camera ? sampleCamera(*camera, time.value()) : std::nullopt;
}

} // namespace playback::editor::keyframe
