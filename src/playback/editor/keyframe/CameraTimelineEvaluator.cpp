#include "CameraTimelineEvaluator.h"
#include "KeyframeTrack.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace playback::editor::keyframe {

namespace {

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

using Vec2 = editing::model::Vec2;
using Vec3 = editing::model::Vec3;

Vec3 add(Vec3 const& left, Vec3 const& right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 subtract(Vec3 const& left, Vec3 const& right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 scale(Vec3 const& value, float amount) { return {value.x * amount, value.y * amount, value.z * amount}; }

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

float eased(float t, editing::model::EasingType type, Vec2 const& control1, Vec2 const& control2) {
    t = clampUnit(t);
    switch (type) {
    case editing::model::EasingType::Hold:
        return 0.0f;
    case editing::model::EasingType::EaseIn:
        return t * t;
    case editing::model::EasingType::EaseOut:
        return 1.0f - (1.0f - t) * (1.0f - t);
    case editing::model::EasingType::EaseInOut:
        return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
    case editing::model::EasingType::CubicBezier:
        return cubicBezierEase(t, control1, control2);
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

Vec3 cubicBezier(
    Vec3 const& p0,
    Vec3 const& p1,
    Vec3 const& p2,
    Vec3 const& p3,
    float       amount
) {
    float const inverse = 1.0f - amount;
    float const a       = inverse * inverse * inverse;
    float const b       = 3.0f * inverse * inverse * amount;
    float const c       = 3.0f * inverse * amount * amount;
    float const d       = amount * amount * amount;
    return add(add(scale(p0, a), scale(p1, b)), add(scale(p2, c), scale(p3, d)));
}

Vec3 catmullRom(Vec3 const& p0, Vec3 const& p1, Vec3 const& p2, Vec3 const& p3, float amount) {
    // The equivalent cubic-Bezier controls preserve Catmull-Rom's endpoint
    // interpolation while keeping the evaluator's path code uniform.
    auto const control1 = add(p1, scale(subtract(p2, p0), 1.0f / 6.0f));
    auto const control2 = subtract(p2, scale(subtract(p3, p1), 1.0f / 6.0f));
    return cubicBezier(p1, control1, control2, p2, amount);
}

Vec3 samplePathPosition(editing::model::CameraPath const& path, long double tick) {
    auto const& points = path.points;
    if (points.size() == 1 || tick <= points.front().tick) return points.front().position;
    if (tick >= points.back().tick) return points.back().position;

    auto const right = std::ranges::lower_bound(points, tick, {}, &editing::model::CameraPathPoint::tick);
    if (right == points.begin() || right == points.end()) return right == points.end() ? points.back().position : points.front().position;

    auto const& previous = *(right - 1);
    auto const& current  = *right;
    auto const  span     = static_cast<long double>(current.tick - previous.tick);
    float const amount   = span <= 0.0L ? 0.0f : clampUnit(static_cast<float>((tick - previous.tick) / span));
    switch (path.type) {
    case editing::model::SplineType::CatmullRom: {
        auto const& before = right - 1 == points.begin() ? previous : *(right - 2);
        auto const& after  = right + 1 == points.end() ? current : *(right + 1);
        return catmullRom(before.position, previous.position, current.position, after.position, amount);
    }
    case editing::model::SplineType::CubicBezier:
        return cubicBezier(
            previous.position,
            add(previous.position, previous.outTangent),
            add(current.position, current.inTangent),
            current.position,
            amount
        );
    case editing::model::SplineType::Linear:
    default:
        return lerp(previous.position, current.position, amount);
    }
}

void applyLimiter(CameraRenderState& state, editing::model::CameraLimiter const& limiter) {
    if (!limiter.enabled) return;
    state.x = std::clamp(state.x, limiter.min.x, limiter.max.x);
    state.y = std::clamp(state.y, limiter.min.y, limiter.max.y);
    state.z = std::clamp(state.z, limiter.min.z, limiter.max.z);
}

void applyShake(CameraRenderState& state, editing::model::CameraShake const& shake, long double tick) {
    if (shake.endTick <= shake.startTick || tick < shake.startTick || tick > shake.endTick) return;
    float const elapsed  = static_cast<float>(tick - static_cast<long double>(shake.startTick));
    float const phase    = elapsed * shake.frequency * 6.2831853071795864769f;
    float const envelope = clampUnit(static_cast<float>(
        (static_cast<long double>(shake.endTick) - tick)
        / static_cast<long double>(shake.endTick - shake.startTick)
    ));
    state.x += std::sin(phase) * shake.positionAmplitude * envelope;
    state.y += std::cos(phase * 1.17f) * shake.positionAmplitude * envelope;
    state.z += std::sin(phase * 0.83f) * shake.positionAmplitude * envelope;
    state.yaw += std::sin(phase * 1.11f) * shake.rotationAmplitude * envelope;
    state.pitch += std::cos(phase * 0.91f) * shake.rotationAmplitude * envelope;
}

} // namespace

CameraTimelineEvaluator::CameraTimelineEvaluator(
    editing::model::EditorStateExt project,
    std::optional<std::string>     cameraOverride,
    std::optional<std::string>     cameraFallback
)
: mProject(std::move(project)),
  mCameraOverride(std::move(cameraOverride)),
  mCameraFallback(std::move(cameraFallback)) {
    for (auto& camera : mProject.cameras) {
        std::ranges::sort(camera.keys, {}, &editing::model::CameraKeyframe::tick);
        if (camera.path) std::ranges::sort(camera.path->points, {}, &editing::model::CameraPathPoint::tick);
        if (camera.kind == editing::model::CameraKind::Keyframe && !camera.keys.empty()) {
            mKeyframeTracks.emplace(camera.id, KeyframeTrack{camera.keys});
        }
    }
}

editing::model::CameraEntity const* CameraTimelineEvaluator::cameraForTick(int64_t tick) const {
    if (mProject.cameras.empty()) return nullptr;

    auto findCamera = [&](std::string const& id) -> editing::model::CameraEntity const* {
        if (id.empty()) return nullptr;
        auto const match = std::ranges::find(mProject.cameras, id, &editing::model::CameraEntity::id);
        return match == mProject.cameras.end() || !editing::model::isCameraRenderable(*match) ? nullptr : &*match;
    };

    if (mCameraOverride && !mCameraOverride->empty()) {
        if (auto const* camera = findCamera(*mCameraOverride)) return camera;
    }

    // Sequence bindings are authoritative when present. A dangling, disabled,
    // or incomplete binding is treated as absent so export can still use its
    // explicit fallback camera.
    for (auto const& segment : mProject.sequence) {
        if (tick < segment.startTick || tick >= segment.endTick || segment.cameraId.empty()) continue;
        if (auto const* camera = findCamera(segment.cameraId)) return camera;
        break;
    }

    if (mCameraFallback) {
        if (auto const* camera = findCamera(*mCameraFallback)) return camera;
    }
    auto const firstRenderable = std::ranges::find_if(mProject.cameras, editing::model::isCameraRenderable);
    return firstRenderable == mProject.cameras.end() ? nullptr : &*firstRenderable;
}

std::optional<CameraRenderState>
CameraTimelineEvaluator::sampleCamera(editing::model::CameraEntity const& camera, long double tick) const {
    if (camera.kind == editing::model::CameraKind::Keyframe && !camera.keys.empty()) {
        auto const track = mKeyframeTracks.find(camera.id);
        if (track == mKeyframeTracks.end()) return std::nullopt;
        auto state = track->second.sample(tick);
        if (!state) return std::nullopt;
        applyLimiter(*state, camera.limiter.value_or(editing::model::CameraLimiter{}));
        applyShake(*state, camera.shake.value_or(editing::model::CameraShake{}), tick);
        return state;
    }

    if (camera.kind == editing::model::CameraKind::Path && camera.path && !camera.path->points.empty()) {
        auto const position = samplePathPosition(*camera.path, tick);
        CameraRenderState state{
            position.x,
            position.y,
            position.z,
            camera.path->defaultRotation.x,
            camera.path->defaultRotation.y,
            0.0f,
            std::clamp(camera.path->defaultFov, 1.0f, 179.0f),
        };
        applyLimiter(state, camera.limiter.value_or(editing::model::CameraLimiter{}));
        applyShake(state, camera.shake.value_or(editing::model::CameraShake{}), tick);
        return state;
    }
    if (camera.kind == editing::model::CameraKind::Rig && camera.rig) {
        CameraRenderState state{
            camera.rig->basePosition.x,
            camera.rig->basePosition.y,
            camera.rig->basePosition.z,
            camera.rig->baseRotation.x,
            camera.rig->baseRotation.y,
            0.0f,
            std::clamp(camera.rig->baseFov, 1.0f, 179.0f),
        };
        for (auto const& segment : camera.rig->segments) {
            if (segment.endTick <= segment.startTick || tick < segment.startTick || tick > segment.endTick) continue;
            float const amount = eased(
                static_cast<float>((tick - segment.startTick) / static_cast<long double>(segment.endTick - segment.startTick)),
                segment.easing,
                {0.42f, 0.0f},
                {0.58f, 1.0f}
            );
            float const value = segment.startValue + (segment.endValue - segment.startValue) * amount;
            switch (segment.motion) {
            case editing::model::RigMotion::Dolly: state.z += value; break;
            case editing::model::RigMotion::Truck: state.x += value; break;
            case editing::model::RigMotion::Pedestal: state.y += value; break;
            case editing::model::RigMotion::Pan: state.yaw += value; break;
            case editing::model::RigMotion::Tilt: state.pitch += value; break;
            case editing::model::RigMotion::Roll: state.roll += value; break;
            case editing::model::RigMotion::Zoom: state.fov = std::clamp(state.fov + value, 1.0f, 179.0f); break;
            case editing::model::RigMotion::Follow: break;
            }
        }
        applyLimiter(state, camera.limiter.value_or(editing::model::CameraLimiter{}));
        applyShake(state, camera.shake.value_or(editing::model::CameraShake{}), tick);
        return state;
    }
    if (camera.kind == editing::model::CameraKind::Preset && camera.preset) {
        CameraRenderState state{
            camera.preset->offset.x,
            camera.preset->offset.y,
            camera.preset->offset.z,
            camera.preset->rotation.x,
            camera.preset->rotation.y,
            0.0f,
            std::clamp(camera.preset->fov, 1.0f, 179.0f),
        };
        if (camera.preset->kind == editing::model::PresetKind::Orbit) {
            float const phase = static_cast<float>(
                (tick - static_cast<long double>(camera.preset->orbitPhaseTick)) * camera.preset->orbitSpeed
            ) * 0.05f;
            state.x = camera.preset->orbitCenter.x + std::cos(phase) * camera.preset->orbitRadius;
            state.y = camera.preset->orbitCenter.y + camera.preset->offset.y;
            state.z = camera.preset->orbitCenter.z + std::sin(phase) * camera.preset->orbitRadius;
            state.yaw = -phase * 57.29577951308232f + 90.0f;
        }
        applyLimiter(state, camera.limiter.value_or(editing::model::CameraLimiter{}));
        applyShake(state, camera.shake.value_or(editing::model::CameraShake{}), tick);
        return state;
    }
    return std::nullopt;
}

std::optional<CameraRenderState>
CameraTimelineEvaluator::sample(functions::render::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto const* camera = cameraForTick(time.floorTick());
    return camera ? sampleCamera(*camera, time.value()) : std::nullopt;
}

std::optional<CameraRenderState> CameraTimelineEvaluator::sampleCameraById(
    std::string_view                           cameraId,
    functions::render::ReplaySampleTime const& time
) const {
    if (cameraId.empty() || !time.isValid()) return std::nullopt;
    auto const camera =
        std::ranges::find_if(mProject.cameras, [&](auto const& candidate) { return candidate.id == cameraId; });
    if (camera == mProject.cameras.end() || !editing::model::isCameraRenderable(*camera)) return std::nullopt;
    return sampleCamera(*camera, time.value());
}

std::optional<CameraPathSampleRange> CameraTimelineEvaluator::sampleCameraPathAround(
    std::string_view                           cameraId,
    functions::render::ReplaySampleTime const& time,
    size_t                                     maxSamples
) const {
    if (cameraId.empty() || !time.isValid() || maxSamples == 0) return std::nullopt;
    auto const camera =
        std::ranges::find_if(mProject.cameras, [&](auto const& candidate) { return candidate.id == cameraId; });
    if (camera == mProject.cameras.end() || !editing::model::isCameraRenderable(*camera)) return std::nullopt;

    int startTick{};
    int endTick{};
    if (camera->kind == editing::model::CameraKind::Keyframe) {
        auto const track = mKeyframeTracks.find(camera->id);
        if (track == mKeyframeTracks.end()) return std::nullopt;
        auto const range = track->second.surroundingRange(time.value());
        if (!range) return std::nullopt;
        startTick   = range->startTick;
        endTick     = range->endTick;
        auto samples = track->second.sampleRange(startTick, endTick, maxSamples);
        if (samples.empty()) return std::nullopt;
        long double const span = static_cast<long double>(endTick) - startTick;
        size_t const count      = samples.size();
        for (size_t index = 0; index < count; ++index) {
            long double const sampleTick = count <= 1
                                             ? static_cast<long double>(startTick)
                                             : static_cast<long double>(startTick)
                                                     + span * static_cast<long double>(index)
                                                           / static_cast<long double>(count - 1);
            applyLimiter(samples[index], camera->limiter.value_or(editing::model::CameraLimiter{}));
            applyShake(samples[index], camera->shake.value_or(editing::model::CameraShake{}), sampleTick);
        }
        return CameraPathSampleRange{startTick, endTick, std::move(samples)};
    }

    if (camera->kind == editing::model::CameraKind::Path && camera->path && !camera->path->points.empty()) {
        auto const& points = camera->path->points;
        auto right = std::ranges::upper_bound(points, time.value(), {}, &editing::model::CameraPathPoint::tick);
        size_t rightIndex = right == points.end() ? points.size() - 1 : static_cast<size_t>(right - points.begin());
        size_t leftIndex  = rightIndex == 0 ? 0 : rightIndex - 1;
        startTick = points[leftIndex == 0 ? 0 : leftIndex - 1].tick;
        endTick   = points[std::min(points.size() - 1, rightIndex + 1)].tick;
    } else {
        constexpr int LocalPreviewRadius = 100;
        int64_t const center = time.floorTick();
        startTick = static_cast<int>(std::max<int64_t>(0, center - LocalPreviewRadius));
        endTick   = static_cast<int>(std::min<int64_t>(std::numeric_limits<int>::max(), center + LocalPreviewRadius));
    }

    size_t const count = std::max<size_t>(2, maxSamples);
    std::vector<CameraRenderState> samples;
    samples.reserve(count);
    long double const span = static_cast<long double>(endTick) - startTick;
    for (size_t index = 0; index < count; ++index) {
        long double const amount = static_cast<long double>(index) / static_cast<long double>(count - 1);
        if (auto state = sampleCamera(*camera, static_cast<long double>(startTick) + span * amount)) {
            samples.push_back(*state);
        }
    }
    if (samples.empty()) return std::nullopt;
    return CameraPathSampleRange{startTick, endTick, std::move(samples)};
}

} // namespace playback::editor::keyframe
